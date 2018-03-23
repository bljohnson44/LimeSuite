#include "FPGA_common.h"
#include "IConnection.h"
#include "LMS64CProtocol.h"
#include <ciso646>
#include <vector>
#include <map>
#include <math.h>
#include <assert.h>
#include <thread>
#include "Logger.h"
using namespace std;

namespace lime
{

// 0x000A
const int RX_EN = 1; //controls both receiver and transmitter
const int TX_EN = 1 << 1; //used for wfm playback from fpga
const int STREAM_LOAD = 1 << 2;

// 0x0009
const int SMPL_NR_CLR = 1; // rising edge clears
const int TXPCT_LOSS_CLR = 1 << 1; // 0 - normal operation, 1-clear

const uint16_t PLLCFG_START = 0x1;
const uint16_t PHCFG_START = 0x2;
const uint16_t PLLRST_START = 0x4;
const uint16_t PHCFG_UPDN = 1 << 13;
const uint16_t PHCFG_MODE = 1 << 14;

const uint16_t busyAddr = 0x0021;


void FPGA::SetConnection(IConnection* conn)
{
    connection = conn;
}

IConnection* FPGA::GetConnection() const
{
    return connection;
}

int FPGA::StartStreaming()
{
    uint16_t interface_ctrl_000A;
    int status = connection->ReadRegister(0x000A, interface_ctrl_000A);
    if (status != 0)
        return status;
    uint32_t value = RX_EN;
    status = connection->WriteRegister(0x000A, interface_ctrl_000A | value);
    return status;
}

int FPGA::StopStreaming()
{
    uint16_t interface_ctrl_000A;
    int status = connection->ReadRegister(0x000A, interface_ctrl_000A);
    if (status != 0)
        return status;
    uint32_t value = ~(RX_EN | TX_EN);
    connection->WriteRegister(0x000A, interface_ctrl_000A & value);
    return status;
}

int FPGA::ResetTimestamp()
{
    int status;
#ifndef NDEBUG
    uint16_t interface_ctrl_000A;
    status = connection->ReadRegister(0x000A, interface_ctrl_000A);
    if (status != 0)
        return 0;

    if ((interface_ctrl_000A & RX_EN))
        return ReportError(EPERM, "Streaming must be stopped to reset timestamp");

#endif // NDEBUG
    //reset hardware timestamp to 0
    uint16_t interface_ctrl_0009;
    status = connection->ReadRegister(0x0009, interface_ctrl_0009);
    if (status != 0)
        return 0;
    uint32_t value = (TXPCT_LOSS_CLR | SMPL_NR_CLR);
    connection->WriteRegister(0x0009, interface_ctrl_0009 & ~(value));
    connection->WriteRegister(0x0009, interface_ctrl_0009 | value);
    connection->WriteRegister(0x0009, interface_ctrl_0009 & ~value);
    return status;
}

int FPGA::SetPllClock(int clockIndex, int nSteps, bool waitLock, uint16_t &reg23val)
{
    const auto timeout = chrono::seconds(3);
    auto t1 = chrono::high_resolution_clock::now();
    auto t2 = t1;
    vector<uint32_t> addrs;
    vector<uint32_t> values;
    addrs.push_back(0x0023); values.push_back(reg23val & ~PLLCFG_START);
    addrs.push_back(0x0024); values.push_back(abs(nSteps)); //CNT_PHASE
    int cnt_ind = (clockIndex + 2) & 0x1F; //C0 index 2, C1 index 3...
    reg23val &= ~(0xF<<8);
    reg23val &= ~(PHCFG_MODE);
    reg23val = reg23val | (cnt_ind << 8);
    if(nSteps >= 0)
        reg23val |= PHCFG_UPDN;
    else
        reg23val &= ~PHCFG_UPDN;
    addrs.push_back(0x0023); values.push_back(reg23val); //PHCFG_UpDn, CNT_IND
    addrs.push_back(0x0023); values.push_back(reg23val | PHCFG_START);

    if(connection->WriteRegisters(addrs.data(), values.data(), values.size()) != 0)
        ReportError(EIO, "SetPllFrequency: PHCFG, failed to write registers");
    addrs.clear(); values.clear();

    bool done = false;
    uint8_t errorCode = 0;
    t1 = chrono::high_resolution_clock::now();
    if(waitLock) do
    {
        uint16_t statusReg;
        connection->ReadRegister(busyAddr, statusReg);
        done = statusReg & 0x1;
        errorCode = (statusReg >> 7) & 0xFF;
        t2 = chrono::high_resolution_clock::now();
        std::this_thread::sleep_for(chrono::milliseconds(10));
    } while(!done && errorCode == 0 && (t2-t1) < timeout);
    if(t2 - t1 > timeout)
        return ReportError(ENODEV, "SetPllFrequency: PHCFG timeout, busy bit is still 1");
    if(errorCode != 0)
        return ReportError(EBUSY, "SetPllFrequency: error configuring PHCFG");
    addrs.push_back(0x0023); values.push_back(reg23val & ~PHCFG_START);
    if(connection->WriteRegisters(addrs.data(), values.data(), values.size()) != 0)
        ReportError(EIO, "SetPllFrequency: configure FPGA PLL, failed to write registers");
    return 0;
}

/** @brief Configures board FPGA clocks
@param serPort communications port
@param pllIndex index of FPGA pll
@param clocks list of clocks to configure
@param clocksCount number of clocks to configure
@return 0-success, other-failure
*/
int FPGA::SetPllFrequency(const uint8_t pllIndex, const double inputFreq, FPGA_PLL_clock* clocks, const uint8_t clockCount)
{
    auto t1 = chrono::high_resolution_clock::now();
    auto t2 = t1;
    const auto timeout = chrono::seconds(3);
    if(not connection)
        return ReportError(ENODEV, "ConfigureFPGA_PLL: connection port is NULL");
    if(not connection->IsOpen())
        return ReportError(ENODEV, "ConfigureFPGA_PLL: configure FPGA PLL, device not connected");
    eLMS_DEV boardType = connection->GetDeviceInfo().deviceName == GetDeviceName(LMS_DEV_LIMESDR_QPCIE) ? LMS_DEV_LIMESDR_QPCIE : LMS_DEV_UNKNOWN;

    if(pllIndex > 15)
        ReportError(ERANGE, "SetPllFrequency: PLL index(%i) out of range [0-15]", pllIndex);

    //check if all clocks are above 5MHz
    const double PLLlowerLimit = 5e6;
    if(inputFreq < PLLlowerLimit)
        return ReportError(ERANGE, "SetPllFrequency: input frequency must be >=%g MHz", PLLlowerLimit/1e6);
    for(int i=0; i<clockCount; ++i)
        if(clocks[i].outFrequency < PLLlowerLimit && not clocks[i].bypass)
            return ReportError(ERANGE, "SetPllFrequency: clock(%i) must be >=%g MHz", i, PLLlowerLimit/1e6);

    //disable direct clock source
    uint16_t drct_clk_ctrl_0005 = 0;
    connection->ReadRegister(0x0005, drct_clk_ctrl_0005);
    connection->WriteRegister(0x0005, drct_clk_ctrl_0005 & ~(1 << pllIndex));

    uint16_t reg23val = 0;
    if(connection->ReadRegister(0x0003, reg23val) != 0)
        return ReportError(ENODEV, "SetPllFrequency: failed to read register");

    reg23val &= ~(0x1F << 3); //clear PLL index
    reg23val &= ~PLLCFG_START; //clear PLLCFG_START
    reg23val &= ~PHCFG_START; //clear PHCFG
    reg23val &= ~PLLRST_START; //clear PLL reset
    reg23val &= ~PHCFG_UPDN; //clear PHCFG_UpDn
    reg23val |= pllIndex << 3;
    
    uint16_t reg25 = 0x0170;
    connection->ReadRegister(0x0025, reg25);

    uint16_t statusReg;
    bool done = false;
    uint8_t errorCode = 0;
    vector<uint32_t> addrs;
    vector<uint32_t> values;
    addrs.push_back(0x0025); values.push_back(reg25 | 0x80);
    addrs.push_back(0x0023); values.push_back(reg23val); //PLL_IND
    if (clocks->findPhase == false)
    { 
        addrs.push_back(0x0023); values.push_back(reg23val | PLLRST_START);
    }
    connection->WriteRegisters(addrs.data(), values.data(), values.size());
    addrs.clear(); values.clear();

    t1 = chrono::high_resolution_clock::now();
    if(boardType == LMS_DEV_LIMESDR_QPCIE) do //wait for reset to activate
    {
        connection->ReadRegister(busyAddr, statusReg);
        done = statusReg & 0x1;
        errorCode = (statusReg >> 7) & 0xFF;
        std::this_thread::sleep_for(chrono::milliseconds(10));
        t2 = chrono::high_resolution_clock::now();
    } while(not done && errorCode == 0 && (t2-t1) < timeout);
    if(t2 - t1 > timeout)
        return ReportError(ENODEV, "SetPllFrequency: PLLRST timeout, busy bit is still 1");
    if(errorCode != 0)
        return ReportError(EBUSY, "SetPllFrequency: error resetting PLL");

    addrs.push_back(0x0023); values.push_back(reg23val & ~PLLRST_START);

    //configure FPGA PLLs
    const double vcoLimits_Hz[2] = { 600e6, 1300e6 };

    map< unsigned long, int> availableVCOs; //all available frequencies for VCO
    for(int i=0; i<clockCount; ++i)
    {
        unsigned long freq;
        freq = clocks[i].outFrequency*(int(vcoLimits_Hz[0]/clocks[i].outFrequency) + 1);
        while(freq >= vcoLimits_Hz[0] && freq <= vcoLimits_Hz[1])
        {
            //add all output frequency multiples that are in VCO interval
            availableVCOs.insert( pair<unsigned long, int>(freq, 0));
            freq += clocks[i].outFrequency;
        }
    }

    int bestScore = 0; //score shows how many outputs have integer dividers
    //calculate scores for all available frequencies
    for (auto &it : availableVCOs)
    {
        for(int i=0; i<clockCount; ++i)
        {
            if(clocks[i].outFrequency == 0 || clocks[i].bypass)
                continue;

            if( (int(it.first) % int(clocks[i].outFrequency)) == 0)
                it.second = it.second+1;
        }
        if(it.second > bestScore)
        {
            bestScore = it.second;
        }
    }
    int N(0), M(0);
    double bestDeviation = 1e9;
    double Fvco;
    for(auto it : availableVCOs)
    {
        if(it.second == bestScore)
        {
            float coef = (it.first / inputFreq);
            int Ntemp = 1;
            int Mtemp = int(coef + 0.5);
            while(inputFreq / Ntemp > PLLlowerLimit)
            {
                ++Ntemp;
                Mtemp = int(coef*Ntemp + 0.5);
                if(Mtemp > 255)
                {
                    --Ntemp;
                    Mtemp = int(coef*Ntemp + 0.5);
                    break;
                }
            }
            double deviation = fabs(it.first - inputFreq*Mtemp / Ntemp);
            if(deviation <= bestDeviation)
            {
                bestDeviation = deviation;
                Fvco = it.first;
                M = Mtemp;
                N = Ntemp;
            }
        }
    }

    int mlow = M / 2;
    int mhigh = mlow + M % 2;
    Fvco = inputFreq*M/N; //actual VCO freq
    lime::debug("M=%i, N=%i, Fvco=%.3f MHz", M, N, Fvco / 1e6);
    if(Fvco < vcoLimits_Hz[0] || Fvco > vcoLimits_Hz[1])
        return ReportError(ERANGE, "SetPllFrequency: VCO(%g MHz) out of range [%g:%g] MHz", Fvco/1e6, vcoLimits_Hz[0]/1e6, vcoLimits_Hz[1]/1e6);

    uint16_t M_N_odd_byp = (M%2 << 3) | (N%2 << 1);
    if(M == 1)
        M_N_odd_byp |= 1 << 2; //bypass M
    if(N == 1)
        M_N_odd_byp |= 1; //bypass N
    addrs.push_back(0x0026); values.push_back(M_N_odd_byp);
    int nlow = N / 2;
    int nhigh = nlow + N % 2;
    addrs.push_back(0x002A); values.push_back(nhigh << 8 | nlow); //N_high_cnt, N_low_cnt
    addrs.push_back(0x002B); values.push_back(mhigh << 8 | mlow);

    uint16_t c7_c0_odds_byps = 0x5555; //bypass all C
    uint16_t c15_c8_odds_byps = 0x5555; //bypass all C

    //set outputs
    for(int i=0; i<clockCount; ++i)
    {
        int C = int(Fvco / clocks[i].outFrequency + 0.5);
        int clow = C / 2;
        int chigh = clow + C % 2;
        if(i < 8)
        {
            if(not clocks[i].bypass && C != 1)
                c7_c0_odds_byps &= ~(1 << (i*2)); //enable output
            c7_c0_odds_byps |= (C % 2) << (i*2+1); //odd bit
        }
        else
        {
            if(not clocks[i].bypass && C != 1)
                c15_c8_odds_byps &= ~(1 << ((i-8)*2)); //enable output
            c15_c8_odds_byps |= (C % 2) << ((i-8)*2+1); //odd bit
        }
        addrs.push_back(0x002E + i); values.push_back(chigh << 8 | clow);
        clocks[i].rd_actualFrequency = (inputFreq * M / N) / (chigh + clow);
    }
    addrs.push_back(0x0027); values.push_back(c7_c0_odds_byps);
    addrs.push_back(0x0028); values.push_back(c15_c8_odds_byps);
    if (clockCount != 4 || clocks->index == 3)
    {
        addrs.push_back(0x0023); values.push_back(reg23val | PLLCFG_START);
    }
    if(connection->WriteRegisters(addrs.data(), values.data(), values.size()) != 0)
        lime::error("SetPllFrequency: PLL CFG, failed to write registers");
    addrs.clear(); values.clear();

    t1 = chrono::high_resolution_clock::now();
    if(boardType == LMS_DEV_LIMESDR_QPCIE) do //wait for config to activate
    {
        connection->ReadRegister(busyAddr, statusReg);
        done = statusReg & 0x1;
        errorCode = (statusReg >> 7) & 0xFF;
        t2 = chrono::high_resolution_clock::now();
        std::this_thread::sleep_for(chrono::milliseconds(10));
    } while(not done && errorCode == 0 && (t2-t1) < timeout);
    if(t2 - t1 > timeout)
        return ReportError(ENODEV, "SetPllFrequency: PLLCFG timeout, busy bit is still 1");
    if(errorCode != 0)
        return ReportError(EBUSY, "SetPllFrequency: error configuring PLLCFG");

    for(int i=0; i<clockCount; ++i)
    {
        int C = int(Fvco / clocks[i].outFrequency + 0.5);
        float fOut_MHz = inputFreq/1e6;
        float Fstep_us = 1 / (8 * fOut_MHz*C);
        float Fstep_deg = (360 * Fstep_us) / (1 / fOut_MHz);
        if (clocks[i].findPhase == false)
        {
           const int nSteps = 0.49 + clocks[i].phaseShift_deg  / Fstep_deg;
           SetPllClock(clocks[i].index,nSteps, boardType, reg23val);
        }
        else
        {
            const int nSteps = (360.0 / Fstep_deg)-0.5;
            const auto timeout = chrono::seconds(3);
            t1 = chrono::high_resolution_clock::now();
            t2 = t1;
            addrs.clear(); values.clear();
            int cnt_ind = (clocks[i].index + 2) & 0x1F; //C0 index 2, C1 index 3...
            reg23val &= ~PLLCFG_START;
            reg23val &= ~(0xF << 8);
            reg23val |= (cnt_ind << 8);
            reg23val |= PHCFG_UPDN;
            reg23val |= PHCFG_MODE;

            addrs.push_back(0x0023); values.push_back(reg23val); //PHCFG_UpDn, CNT_IND
            addrs.push_back(0x0024); values.push_back(abs(nSteps)); //CNT_PHASE
            addrs.push_back(0x0023); values.push_back(reg23val | PHCFG_START);

            if (connection->WriteRegisters(addrs.data(), values.data(), values.size()) != 0)
                lime::error( "SetPllFrequency: find phase, failed to write registers");
            addrs.clear(); values.clear();

            bool done = false;
            bool error = false;

            t1 = chrono::high_resolution_clock::now();
            do
            {
                uint16_t statusReg;
                connection->ReadRegister(busyAddr, statusReg);
                done = statusReg & 0x4;
                error = statusReg & 0x08;
                t2 = chrono::high_resolution_clock::now();
                std::this_thread::sleep_for(chrono::milliseconds(10));
            } while (!done && (t2 - t1) < timeout);
            if (!done && t2 - t1 > timeout)
                lime::error("SetPllFrequency: timeout, busy bit is still 1");
            if (error)
                lime::error("SetPllFrequency: error configuring phase");
            addrs.push_back(0x0023); values.push_back(reg23val & ~PHCFG_START);
            if (connection->WriteRegisters(addrs.data(), values.data(), values.size()) != 0)
                lime::error("SetPllFrequency: configure FPGA PLL, failed to write registers");
            return (!done) || error ? -1 : 0;
        }
    }
    return 0;
}

int FPGA::SetDirectClocking(int clockIndex)
{
    if(not connection)
        return ReportError(ENODEV, "SetDirectClocking: connection port is NULL");
    if(not connection->IsOpen())
        return ReportError(ENODEV, "SetDirectClocking: device not connected");

    uint16_t drct_clk_ctrl_0005 = 0;
    connection->ReadRegister(0x0005, drct_clk_ctrl_0005);
    uint16_t drct_clk_ctrl_0006;
    connection->ReadRegister(0x0006, drct_clk_ctrl_0006);
    vector<uint32_t> addres;
    vector<uint32_t> values;
    //enable direct clocking
    addres.push_back(0x0005); values.push_back(drct_clk_ctrl_0005 | (1 << clockIndex));
    if(connection->WriteRegisters(addres.data(), values.data(), values.size()) != 0)
        return ReportError(EIO, "SetDirectClocking: failed to write registers");
    return 0;
}

/** @brief Parses FPGA packet payload into samples
*/
int FPGA::FPGAPacketPayload2Samples(const uint8_t* buffer, int bufLen, bool mimo, bool compressed, complex16_t** samples)
{
    if(compressed) //compressed samples
    {
        int16_t sample;
        int collected = 0;
        for(int b=0; b<bufLen;collected++)
        {
            //I sample
            sample = buffer[b++];
            sample |= (buffer[b] << 8);
            sample <<= 4;
            samples[0][collected].i = sample >> 4;
            //Q sample
            sample =  buffer[b++];
            sample |= buffer[b++] << 8;
            samples[0][collected].q = sample >> 4;
            if (mimo)
            {
                //I sample
                sample = buffer[b++];
                sample |= (buffer[b] << 8);
                sample <<= 4;
                samples[1][collected].i = sample >> 4;
                //Q sample
                sample =  buffer[b++];
                sample |= buffer[b++] << 8;
                samples[1][collected].q = sample >> 4;
            }
        }
        return collected;
    }

    if (mimo) //uncompressed samples
    {
        complex16_t* ptr = (complex16_t*)buffer;
        const int collected = bufLen/sizeof(complex16_t)/2;
        for(int i=0; i<collected;i++)
        {
            samples[0][i] = *ptr++;
            samples[1][i] = *ptr++;
        }
        return collected;
    }

    memcpy(samples[0],buffer,bufLen);
    return bufLen/sizeof(complex16_t);
}

int FPGA::Samples2FPGAPacketPayload(const complex16_t* const* samples, int samplesCount, bool mimo, bool compressed, uint8_t* buffer)
{
    if(compressed)
    {
        int b=0;
        for(int src=0; src<samplesCount; ++src)
        {
            buffer[b++] = samples[0][src].i;
            buffer[b++] = ((samples[0][src].i >> 8) & 0x0F) | (samples[0][src].q << 4);
            buffer[b++] = samples[0][src].q >> 4;
            if (mimo)
            {
                buffer[b++] = samples[1][src].i;
                buffer[b++] = ((samples[1][src].i >> 8) & 0x0F) | (samples[1][src].q << 4);
                buffer[b++] = samples[1][src].q >> 4;
            }
        }
        return b;
    }

    if (mimo)
    {
        complex16_t* ptr = (complex16_t*)buffer;
        for(int src=0; src<samplesCount; ++src)
        {
            *ptr++ = samples[0][src];
            *ptr++ = samples[1][src];
        }
        return samplesCount*2*sizeof(complex16_t);
    }
    memcpy(buffer,samples[0],samplesCount*sizeof(complex16_t));
    return samplesCount*sizeof(complex16_t);
}

int FPGA::UploadWFM(const void* const* samples, uint8_t chCount, size_t sample_count, StreamConfig::StreamDataFormat format, int epIndex)
{
    const int samplesInPkt = samples16InPkt;
    connection->WriteRegister(0xFFFF, 1 << epIndex);
    connection->WriteRegister(0x000C, chCount == 2 ? 0x3 : 0x1); //channels 0,1
    connection->WriteRegister(0x000E, 0x2); //12bit samples

    uint16_t regValue = 0;
    connection->ReadRegister(0x000D,regValue);
    regValue |= 0x4;
    connection->WriteRegister(0x000D, regValue);

    lime::FPGA_DataPacket pkt;
    size_t samplesUsed = 0;
    int cnt = sample_count;

    const complex16_t* const* src = (const complex16_t* const*)samples;
    const lime::complex16_t** batch = new const lime::complex16_t*[chCount];
    lime::complex16_t** samplesShort = new lime::complex16_t*[chCount];
    for(unsigned i=0; i<chCount; ++i)
        samplesShort[i] = nullptr;

    if (format == StreamConfig::FMT_INT16)
    {
        for(unsigned i=0; i<chCount; ++i)
            samplesShort[i] = new lime::complex16_t[sample_count];
        for (int ch = 0; ch < chCount; ch++)
            for(size_t i=0; i < sample_count; ++i)
            {
                samplesShort[ch][i].i = src[ch][i].i >> 4;
                samplesShort[ch][i].q = src[ch][i].q >> 4;
            }
        src = samplesShort;
    }
    else if(format == StreamConfig::FMT_FLOAT32)
    {
        const float mult = 2047.5f;
        for(unsigned i=0; i<chCount; ++i)
            samplesShort[i] = new lime::complex16_t[sample_count];

        const float* const* samplesFloat = (const float* const*)samples;
        for (int ch = 0; ch < chCount; ch++)
            for(size_t i=0; i < sample_count; ++i)
            {
                samplesShort[ch][i].i = samplesFloat[ch][2*i]*mult;
                samplesShort[ch][i].q = samplesFloat[ch][2*i+1]*mult;
            }
        src = samplesShort;
    }

    while(cnt > 0)
    {
        pkt.counter = 0;
        pkt.reserved[0] = 0;
        int samplesToSend = cnt > samplesInPkt/chCount ? samplesInPkt/chCount : cnt;

        for(unsigned i=0; i<chCount; ++i)
            batch[i] = &src[i][samplesUsed];
        samplesUsed += samplesToSend;

        int bufPos = Samples2FPGAPacketPayload(batch, samplesToSend, chCount==2, true, pkt.data);
        int payloadSize = (bufPos / 4) * 4;
        if(bufPos % 4 != 0)
            lime::warning("Packet samples count not multiple of 4");
        pkt.reserved[2] = (payloadSize >> 8) & 0xFF; //WFM loading
        pkt.reserved[1] = payloadSize & 0xFF; //WFM loading
        pkt.reserved[0] = 0x1 << 5; //WFM loading

        long bToSend = 16+payloadSize;
        if (connection->SendData((const char*)&pkt,bToSend,epIndex,500)!=bToSend)
            break;
        cnt -= samplesToSend;
    }
    delete[] batch;
    for(unsigned i=0; i<chCount; ++i)
        if (samplesShort[i])
            delete [] samplesShort[i];
    delete[] samplesShort;

    /*Give some time to load samples to FPGA*/
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    connection->AbortSending(epIndex);
    if(cnt == 0)
        return 0;
    else
        return ReportError(-1, "Failed to upload waveform");
}


/** @brief Configures FPGA PLLs to LimeLight interface frequency
*/
int FPGA::SetInterfaceFreq(double txRate_Hz, double rxRate_Hz, double txPhase, double rxPhase, int channel)
{
    lime::FPGA::FPGA_PLL_clock clocks[2];
    int status = 0;
    if  (rxRate_Hz >= 5e6)
    { 
        clocks[0].index = 0;
        clocks[0].outFrequency = rxRate_Hz;
        clocks[1].index = 1;
        clocks[1].outFrequency = rxRate_Hz;
        clocks[1].phaseShift_deg = rxPhase;
        status = SetPllFrequency(1, rxRate_Hz, clocks, 2);
    }
    else
        status = SetDirectClocking(1);

    if (txRate_Hz >= 5e6)
    { 
        clocks[0].index = 0;
        clocks[0].outFrequency = txRate_Hz;
        clocks[1].index = 1;
        clocks[1].outFrequency = txRate_Hz;
        clocks[1].phaseShift_deg = txPhase;
        status |= SetPllFrequency(0, txRate_Hz, clocks, 2);
    }
    else
        status |= SetDirectClocking(0);
    return status;
}

/** @brief Configures FPGA PLLs to LimeLight interface frequency
*/
int FPGA::SetInterfaceFreq(double txRate_Hz, double rxRate_Hz, int channel)
{
    const int pll_ind = (channel == 1) ? 2 : 0;
    int status = 0;
    uint32_t reg20;
    const double rxPhC1 =  89.46;
    const double rxPhC2 =  1.24e-6;
    const double txPhC1 =  89.61;
    const double txPhC2 =  2.71e-7;

    const std::vector<uint32_t> spiAddr = { 0x021, 0x022, 0x023, 0x024, 0x027, 0x02A,
                                            0x400, 0x40C, 0x40B, 0x400, 0x40B, 0x400};
    const int bakRegCnt = spiAddr.size() - 4;
    
    bool phaseSearch = false;
    //if (!(mStreamers.size() > channel && (mStreamers[channel]->rxRunning || mStreamers[channel]->txRunning)))
    if(rxRate_Hz >= 5e6 && txRate_Hz >= 5e6)
    {
        uint32_t addr[3] = {0, 1, 2};
        uint32_t vals[3];
        connection->ReadRegisters(addr,vals,3);
        if ((vals[0]==0xE && vals[1]>1 && vals[2] > 0xE)||(vals[0]==0xF && vals[1]>1 && vals[2] > 6))
            phaseSearch = true;
    }

    if (!phaseSearch)
        return SetInterfaceFreq(txRate_Hz, rxRate_Hz, txPhC1 + txPhC2 * txRate_Hz, rxPhC1 + rxPhC2 * rxRate_Hz, 0);

    std::vector<uint32_t> dataRdA;
    std::vector<uint32_t> dataRdB;
    std::vector<uint32_t> dataWr;

    dataWr.resize(spiAddr.size());
    dataRdA.resize(spiAddr.size());
    dataRdB.resize(spiAddr.size());
    //backup registers
    dataWr[0] = (uint32_t(0x0020) << 16);
    connection->ReadLMS7002MSPI(dataWr.data(), &reg20, 1, channel);
        
    dataWr[0] = (1 << 31) | (uint32_t(0x0020) << 16) | 0xFFFD; //msbit 1=SPI write
    connection->WriteLMS7002MSPI(dataWr.data(), 1, channel);

    for (int i = 0; i < bakRegCnt; ++i)
        dataWr[i] = (spiAddr[i] << 16);
    connection->ReadLMS7002MSPI(dataWr.data(),dataRdA.data(), bakRegCnt, channel);

    dataWr[0] = (1 << 31) | (uint32_t(0x0020) << 16) | 0xFFFE; //msbit 1=SPI write
    connection->WriteLMS7002MSPI(dataWr.data(), 1, channel);

    for (int i = 0; i < bakRegCnt; ++i)
        dataWr[i] = (spiAddr[i] << 16);
    connection->ReadLMS7002MSPI(dataWr.data(), dataRdB.data(), bakRegCnt, channel);

    dataWr[0] = (1 << 31) | (uint32_t(0x0020) << 16) | 0xFFFF; //msbit 1=SPI write
    connection->WriteLMS7002MSPI(dataWr.data(), 1, channel);

    {
        const std::vector<uint32_t> spiData = { 0x0E9F, 0x0FFF, 0x5550, 0xE4E4, 0xE4E4, 0x0086,
                                                0x028D, 0x00FF, 0x5555, 0x02CD, 0xAAAA, 0x02ED};
        //Load test config
        const int setRegCnt = spiData.size();
        for (int i = 0; i < setRegCnt; ++i)
            dataWr[i] = (1 << 31) | (uint32_t(spiAddr[i]) << 16) | spiData[i]; //msbit 1=SPI write
        connection->WriteLMS7002MSPI(dataWr.data(), setRegCnt, channel);
    }

    lime::FPGA::FPGA_PLL_clock clocks[2];
    clocks[0].index = 1;
    clocks[0].outFrequency = rxRate_Hz;
    clocks[0].phaseShift_deg = rxPhC1 + rxPhC2 * rxRate_Hz;
    clocks[0].findPhase = true;
    clocks[1] = clocks[0];
    if (SetPllFrequency(pll_ind+1, rxRate_Hz, clocks, 2)!=0)
    {
        clocks[0].index = 0;
        clocks[0].phaseShift_deg = 0;
        clocks[0].findPhase = false;
        clocks[1].findPhase = false;
        status = SetPllFrequency(pll_ind+1, rxRate_Hz, clocks, 2);
    }

    {
        const std::vector<uint32_t> spiData = {0x0E9F, 0x0FFF, 0x5550, 0xE4E4, 0xE4E4, 0x0484};
        connection->WriteRegister(0xFFFF, 1 << channel);
        connection->WriteRegister(0x000A, 0x0000);
        //Load test config
        const int setRegCnt = spiData.size();
        for (int i = 0; i < setRegCnt; ++i)
            dataWr[i] = (1 << 31) | (uint32_t(spiAddr[i]) << 16) | spiData[i]; //msbit 1=SPI write
        connection->WriteLMS7002MSPI(dataWr.data(), setRegCnt, channel);
    }

    clocks[0].index = 1;
    clocks[0].outFrequency = txRate_Hz;
    clocks[0].phaseShift_deg = txPhC1 + txPhC2 * txRate_Hz;
    clocks[0].findPhase = true;
    clocks[1] = clocks[0];
    connection->WriteRegister(0x000A, 0x0200);
    if (SetPllFrequency(pll_ind, txRate_Hz, clocks, 2)!=0)
    {
        clocks[0].index = 0;
        clocks[0].phaseShift_deg = 0;
        clocks[0].findPhase = false;
        clocks[1].findPhase = false;
        status |= SetPllFrequency(pll_ind, txRate_Hz, clocks, 2);
    }

    //Restore registers
    dataWr[0] = (1 << 31) | (uint32_t(0x0020) << 16) | 0xFFFD; //msbit 1=SPI write
    connection->WriteLMS7002MSPI(dataWr.data(), 1, channel);
    for (int i = 0; i < bakRegCnt; ++i)
        dataWr[i] = (1 << 31) | (uint32_t(spiAddr[i]) << 16) | dataRdA[i]; //msbit 1=SPI write
    connection->WriteLMS7002MSPI(dataWr.data(), bakRegCnt, channel);
    dataWr[0] = (1 << 31) | (uint32_t(0x0020) << 16) | 0xFFFE; //msbit 1=SPI write
    connection->WriteLMS7002MSPI(dataWr.data(), 1, channel);
    for (int i = 0; i < bakRegCnt; ++i)
        dataWr[i] = (1 << 31) | (uint32_t(spiAddr[i]) << 16) | dataRdB[i]; //msbit 1=SPI write
    connection->WriteLMS7002MSPI(dataWr.data(), bakRegCnt, channel);
    dataWr[0] = (1 << 31) | (uint32_t(0x0020) << 16) | reg20; //msbit 1=SPI write
    connection->WriteLMS7002MSPI(dataWr.data(), 1, channel);
    connection->WriteRegister(0x000A, 0);

    return status;
}

int FPGA::ReadRawStreamData(char* buffer, unsigned length, int epIndex, int timeout_ms)
{
    connection->WriteRegister(0xFFFF, 1 << epIndex);
    StopStreaming();
    connection->ResetStreamBuffers();
    connection->WriteRegister(0x0008, 0x0100 | 0x2);
    connection->WriteRegister(0x0007, 1);
    StartStreaming();
    int totalBytesReceived = connection->ReceiveData(buffer,length, epIndex, timeout_ms);
    StopStreaming();
    connection->AbortReading(epIndex);
    return totalBytesReceived;
}

double FPGA::DetectRefClk(double fx3Clk)
{
    const double fx3Cnt = 16777210;         //fixed fx3 counter in FPGA
    const double clkTbl[] = { 30.72e6, 38.4e6, 40e6, 52e6 };
    const uint32_t addr[] = { 0x61, 0x63 };
    const uint32_t vals[] = { 0x0, 0x0 };
    if (connection->WriteRegisters(addr, vals, 2) != 0)
        return -1;

    auto start = std::chrono::steady_clock::now();
    if (connection->WriteRegister(0x61, 0x4) != 0)
        return -1;

    while (1) //wait for test to finish
    {
        unsigned completed;
        if (connection->ReadRegister(0x65, completed) != 0)
            return -1;
  
        if (completed & 0x4)
            break;

        auto end = std::chrono::steady_clock::now();
        std::chrono::duration<double> elapsed_seconds = end - start;
        if (elapsed_seconds.count() > 0.5) //timeout
            return -1;
    }

    const uint32_t addr2[] = { 0x72, 0x73 };
    uint32_t vals2[2];
    if (connection->ReadRegisters(addr2, vals2, 2) != 0)
        return -1;
    
    double count = (vals2[0] | (vals2[1] << 16)); //cock counter
    count *= fx3Clk / fx3Cnt;   //estimate ref clock based on FX3 Clock
    lime::debug("Estimated reference clock %1.4f MHz", count/1e6);
    unsigned i = 0;
    double delta = 100e6;

    while (i < sizeof(clkTbl) / sizeof(*clkTbl))
        if (delta < fabs(count - clkTbl[i]))
            break;
        else
            delta = fabs(count - clkTbl[i++]);

    lime::info("Reference clock %1.2f MHz", clkTbl[i - 1] / 1e6);
    return clkTbl[i - 1];
}

} //namespace lime
