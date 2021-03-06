/*
    Copyright 2016-2017 StapleButter

    This file is part of melonDS.

    melonDS is free software: you can redistribute it and/or modify it under
    the terms of the GNU General Public License as published by the Free
    Software Foundation, either version 3 of the License, or (at your option)
    any later version.

    melonDS is distributed in the hope that it will be useful, but WITHOUT ANY
    WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
    FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

    You should have received a copy of the GNU General Public License along
    with melonDS. If not, see http://www.gnu.org/licenses/.
*/

#include <stdio.h>
#include <string.h>
#include "NDS.h"
#include "ARM.h"
#include "CP15.h"
#include "DMA.h"
#include "FIFO.h"
#include "GPU.h"
#include "SPI.h"
#include "RTC.h"
#include "Wifi.h"

// derp
namespace SPI_Firmware
{
    extern u8* Firmware;
}


namespace NDS
{

// TODO LIST
// * stick all the variables in a big structure?
//   would make it easier to deal with savestates
// * move ARM9 TCM to the ARM class (closer to the real thing, and handles "DMA can't access TCM" nicely)

SchedEvent SchedBuffer[SCHED_BUF_LEN];
SchedEvent* SchedQueue;

bool NeedReschedule;

ARM* ARM9;
ARM* ARM7;

s32 ARM9Cycles, ARM7Cycles;
s32 CompensatedCycles;
s32 SchedCycles;

u8 ARM9BIOS[0x1000];
u8 ARM7BIOS[0x4000];

u8 MainRAM[0x400000];

u8 SharedWRAM[0x8000];
u8 WRAMCnt;
u8* SWRAM_ARM9;
u8* SWRAM_ARM7;
u32 SWRAM_ARM9Mask;
u32 SWRAM_ARM7Mask;

u8 ARM7WRAM[0x10000];

u8 ARM9ITCM[0x8000];
u32 ARM9ITCMSize;
u8 ARM9DTCM[0x4000];
u32 ARM9DTCMBase, ARM9DTCMSize;

// IO shit
u32 IME[2];
u32 IE[2], IF[2];

u8 PostFlag9;
u8 PostFlag7;
u16 PowerControl9;
u16 PowerControl7;

Timer Timers[8];

DMA* DMAs[8];
u32 DMA9Fill[4];

u16 IPCSync9, IPCSync7;
u16 IPCFIFOCnt9, IPCFIFOCnt7;
FIFO* IPCFIFO9; // FIFO in which the ARM9 writes
FIFO* IPCFIFO7;

u16 DivCnt;
u32 DivNumerator[2];
u32 DivDenominator[2];
u32 DivQuotient[2];
u32 DivRemainder[2];

u32 ROMSPIControl;
u32 ROMControl;
u8 ROMCommand[8];
u8 ROMCurCommand[8];
u32 ROMReadPos, ROMReadSize;

u32 KeyInput;

u16 _soundbias; // temp

bool Running;


void Init()
{
    ARM9 = new ARM(0);
    ARM7 = new ARM(1);

    DMAs[0] = new DMA(0, 0);
    DMAs[1] = new DMA(0, 1);
    DMAs[2] = new DMA(0, 2);
    DMAs[3] = new DMA(0, 3);
    DMAs[4] = new DMA(1, 0);
    DMAs[5] = new DMA(1, 1);
    DMAs[6] = new DMA(1, 2);
    DMAs[7] = new DMA(1, 3);

    IPCFIFO9 = new FIFO(16);
    IPCFIFO7 = new FIFO(16);

    GPU::Init();
    SPI::Init();
    RTC::Init();

    Reset();
}

// temp
void LoadROM()
{
    FILE* f;

    f = fopen("rom/armwrestler.nds", "rb");
    //f = fopen("rom/zorp.nds", "rb");

    u32 bootparams[8];
    fseek(f, 0x20, SEEK_SET);
    fread(bootparams, 8, 4, f);

    printf("ARM9: offset=%08X entry=%08X RAM=%08X size=%08X\n",
           bootparams[0], bootparams[1], bootparams[2], bootparams[3]);
    printf("ARM7: offset=%08X entry=%08X RAM=%08X size=%08X\n",
           bootparams[4], bootparams[5], bootparams[6], bootparams[7]);

    fseek(f, bootparams[0], SEEK_SET);
    for (u32 i = 0; i < bootparams[3]; i+=4)
    {
        u32 tmp;
        fread(&tmp, 4, 1, f);
        ARM9Write32(bootparams[2]+i, tmp);
    }

    fseek(f, bootparams[4], SEEK_SET);
    for (u32 i = 0; i < bootparams[7]; i+=4)
    {
        u32 tmp;
        fread(&tmp, 4, 1, f);
        ARM7Write32(bootparams[6]+i, tmp);
    }

    fclose(f);

    CP15::Write(0x910, 0x0300000A);
    CP15::Write(0x911, 0x00000020);
    CP15::Write(0x100, 0x00050000);

    ARM9->JumpTo(bootparams[1]);
    ARM7->JumpTo(bootparams[5]);
}

void Reset()
{
    FILE* f;
    u32 i;

    f = fopen("bios9.bin", "rb");
    if (!f)
        printf("ARM9 BIOS not found\n");
    else
    {
        fseek(f, 0, SEEK_SET);
        fread(ARM9BIOS, 0x1000, 1, f);

        printf("ARM9 BIOS loaded: %08X\n", ARM9Read32(0xFFFF0000));
        fclose(f);
    }

    f = fopen("bios7.bin", "rb");
    if (!f)
        printf("ARM7 BIOS not found\n");
    else
    {
        fseek(f, 0, SEEK_SET);
        fread(ARM7BIOS, 0x4000, 1, f);

        printf("ARM7 BIOS loaded: %08X\n", ARM7Read32(0x00000000));
        fclose(f);
    }

    memset(MainRAM, 0, 0x400000);
    memset(SharedWRAM, 0, 0x8000);
    memset(ARM7WRAM, 0, 0x10000);
    memset(ARM9ITCM, 0, 0x8000);
    memset(ARM9DTCM, 0, 0x4000);

    MapSharedWRAM(0);

    ARM9ITCMSize = 0;
    ARM9DTCMBase = 0xFFFFFFFF;
    ARM9DTCMSize = 0;

    IME[0] = 0;
    IME[1] = 0;

    PostFlag9 = 0x00;
    PostFlag7 = 0x00;
    PowerControl9 = 0x0001;
    PowerControl7 = 0x0001;

    IPCSync9 = 0;
    IPCSync7 = 0;
    IPCFIFOCnt9 = 0;
    IPCFIFOCnt7 = 0;
    IPCFIFO9->Clear();
    IPCFIFO7->Clear();

    DivCnt = 0;

    ROMSPIControl = 0;
    ROMControl = 0;
    memset(ROMCommand, 0, 8);

    ARM9->Reset();
    ARM7->Reset();
    CP15::Reset();

    memset(Timers, 0, 8*sizeof(Timer));

    for (i = 0; i < 8; i++) DMAs[i]->Reset();
    memset(DMA9Fill, 0, 4*4);

    GPU::Reset();
    SPI::Reset();
    RTC::Reset();
    Wifi::Reset();

    memset(SchedBuffer, 0, sizeof(SchedEvent)*SCHED_BUF_LEN);
    SchedQueue = NULL;

    ARM9Cycles = 0;
    ARM7Cycles = 0;
    SchedCycles = 0;

    KeyInput = 0x007F03FF;

    _soundbias = 0;

    // test
    //LoadROM();
    //LoadFirmware();

    Running = true; // hax
}

static int fnum = 0;
void RunFrame()
{
    s32 framecycles = 560190<<1;
    const s32 maxcycles = 16;

    if (!Running) return; // dorp

    fnum++;
    //printf("frame %d\n", fnum);

    GPU::StartFrame();

    while (Running && framecycles>0)
    {
        s32 cyclestorun = maxcycles;
        if (SchedQueue)
        {
            if (SchedQueue->Delay < cyclestorun)
                cyclestorun = SchedQueue->Delay;
        }

        //CompensatedCycles = ARM9Cycles;
        s32 torun9 = cyclestorun - ARM9Cycles;
        s32 c9 = ARM9->Execute(torun9);
        ARM9Cycles = c9 - torun9;
        //c9 -= CompensatedCycles;

        s32 torun7 = (c9 - ARM7Cycles) & ~1;
        s32 c7 = ARM7->Execute(torun7 >> 1) << 1;
        ARM7Cycles = c7 - torun7;

        RunEvents(c9);
        framecycles -= cyclestorun;
    }
    //printf("frame end\n");
}

SchedEvent* ScheduleEvent(s32 Delay, void (*Func)(u32), u32 Param)
{
    // find a free entry
    u32 entry = -1;
    for (int i = 0; i < SCHED_BUF_LEN; i++)
    {
        if (SchedBuffer[i].Func == NULL)
        {
            entry = i;
            break;
        }
    }

    if (entry == -1)
    {
        printf("!! SCHEDULER BUFFER FULL\n");
        return NULL;
    }

    SchedEvent* evt = &SchedBuffer[entry];
    evt->Func = Func;
    evt->Param = Param;

    Delay += SchedCycles;

    SchedEvent* cur = SchedQueue;
    SchedEvent* prev = NULL;
    for (;;)
    {
        if (cur == NULL) break;
        if (cur->Delay > Delay) break;

        Delay -= cur->Delay;
        prev = cur;
        cur = cur->NextEvent;
    }

    // so, we found it. we insert our event before 'cur'.
    evt->Delay = Delay;

    if (cur == NULL)
    {
        if (prev == NULL)
        {
            // list empty
            SchedQueue = evt;
            evt->PrevEvent = NULL;
            evt->NextEvent = NULL;
        }
        else
        {
            // inserting at the end of the list
            evt->PrevEvent = prev;
            evt->NextEvent = NULL;
            prev->NextEvent = evt;
        }
    }
    else
    {
        evt->NextEvent = cur;
        evt->PrevEvent = cur->PrevEvent;

        if (evt->PrevEvent)
            evt->PrevEvent->NextEvent = evt;
        else
            SchedQueue = evt;

        cur->PrevEvent = evt;
        cur->Delay -= evt->Delay;
    }

    return evt;
}

void CancelEvent(SchedEvent* event)
{
    event->Func = NULL;

    // unlink

    if (event->PrevEvent)
        event->PrevEvent->NextEvent = event->NextEvent;
    else
        SchedQueue = event->NextEvent;

    if (event->NextEvent)
        event->NextEvent->PrevEvent = event->PrevEvent;
}

void RunEvents(s32 cycles)
{
    SchedCycles += cycles;

    while (SchedQueue && SchedQueue->Delay <= SchedCycles)
    {
        void (*func)(u32) = SchedQueue->Func;
        u32 param = SchedQueue->Param;

        SchedQueue->Func = NULL;
        SchedCycles -= SchedQueue->Delay;

        SchedQueue = SchedQueue->NextEvent;
        if (SchedQueue) SchedQueue->PrevEvent = NULL;

        func(param);
    }
}

void CompensateARM7()
{return;
    s32 c9 = ARM9->Cycles - CompensatedCycles;
    CompensatedCycles = ARM9->Cycles;

    s32 c7 = ARM7->Execute((c9 - ARM7Cycles) >> 1) << 1;
    ARM7Cycles = c7 - c9;

    RunEvents(c9);
}


void PressKey(u32 key)
{
    KeyInput &= ~(1 << key);
}

void ReleaseKey(u32 key)
{
    KeyInput |= (1 << key);
}


void Halt()
{
    printf("Halt()\n");
    Running = false;
}


void MapSharedWRAM(u8 val)
{
    WRAMCnt = val;

    switch (WRAMCnt & 0x3)
    {
    case 0:
        SWRAM_ARM9 = &SharedWRAM[0];
        SWRAM_ARM9Mask = 0x7FFF;
        SWRAM_ARM7 = NULL;
        SWRAM_ARM7Mask = 0;
        break;

    case 1:
        SWRAM_ARM9 = &SharedWRAM[0x4000];
        SWRAM_ARM9Mask = 0x3FFF;
        SWRAM_ARM7 = &SharedWRAM[0];
        SWRAM_ARM7Mask = 0x3FFF;
        break;

    case 2:
        SWRAM_ARM9 = &SharedWRAM[0];
        SWRAM_ARM9Mask = 0x3FFF;
        SWRAM_ARM7 = &SharedWRAM[0x4000];
        SWRAM_ARM7Mask = 0x3FFF;
        break;

    case 3:
        SWRAM_ARM9 = NULL;
        SWRAM_ARM9Mask = 0;
        SWRAM_ARM7 = &SharedWRAM[0];
        SWRAM_ARM7Mask = 0x7FFF;
        break;
    }
}


void TriggerIRQ(u32 cpu, u32 irq)
{
    irq = 1 << irq;
    IF[cpu] |= irq;

    // this is redundant
    if (!(IME[cpu] & 0x1)) return;
    (cpu?ARM7:ARM9)->TriggerIRQ();
}

bool HaltInterrupted(u32 cpu)
{
    if (cpu == 0)
    {
        if (!(IME[0] & 0x1))
            return false;
    }

    if (IF[cpu] & IE[cpu])
        return true;

    return false;
}



const s32 TimerPrescaler[4] = {2, 128, 512, 2048};

void TimerIncrement(u32 param)
{
    Timer* timer = &Timers[param];

    u32 tid = param & 0x3;
    u32 cpu = param >> 2;

    for (;;)
    {
        timer->Counter++;

        if (tid == (param&0x3))
            timer->Event = ScheduleEvent(TimerPrescaler[timer->Control&0x3], TimerIncrement, param);

        if (timer->Counter == 0)
        {
            timer->Counter = timer->Reload;

            if (timer->Control & (1<<6))
            {
                TriggerIRQ(cpu, IRQ_Timer0 + tid);
                //if (cpu==1) printf("Timer%d IRQ %04X\n", tid, timer->Control);
            }

            // cascade
            if (tid == 3)
                break;
            timer++;
            if ((timer->Control & 0x84) != 0x84)
                break;
            tid++;
            continue;
        }

        break;
    }
}

void TimerStart(u32 id, u16 cnt)
{
    Timer* timer = &Timers[id];
    u16 curstart = timer->Control & (1<<7);
    u16 newstart = cnt & (1<<7);

    timer->Control = cnt;

    if ((!curstart) && newstart)
    {
        timer->Counter = timer->Reload;

        // start the timer, if it's not a cascading timer
        if (!(cnt & (1<<2)))
            timer->Event = ScheduleEvent(TimerPrescaler[cnt&0x3], TimerIncrement, id);
        else
            timer->Event = NULL;
    }
    else if (curstart && (!newstart))
    {
        if (timer->Event)
            CancelEvent(timer->Event);
    }
}



void ROMEndTransfer(u32 cpu)
{
    ROMControl &= ~(1<<23);
    ROMControl &= ~(1<<31);

    if (ROMSPIControl & (1<<14))
        TriggerIRQ(cpu, IRQ_CartSendDone);
}

void ROMStartTransfer(u32 cpu)
{
    u32 datasize = (ROMControl >> 24) & 0x7;
    if (datasize == 7)
        datasize = 4;
    else if (datasize > 0)
        datasize = 0x100 << datasize;

    //datasize += (ROMControl & 0x1FFF); // KEY1 gap

    ROMReadPos = 0;
    ROMReadSize = datasize;

    *(u32*)&ROMCurCommand[0] = *(u32*)&ROMCommand[0];
    *(u32*)&ROMCurCommand[4] = *(u32*)&ROMCommand[4];

    printf("ROM COMMAND %04X %08X %02X%02X%02X%02X%02X%02X%02X%02X SIZE %04X\n",
           ROMSPIControl, ROMControl,
           ROMCommand[0], ROMCommand[1], ROMCommand[2], ROMCommand[3],
           ROMCommand[4], ROMCommand[5], ROMCommand[6], ROMCommand[7],
           datasize);

    ROMControl |= (1<<23);

    if (datasize == 0)
    {
        // hax
        /*if (ROMCommand[0] == 0xBA)
            ScheduleEvent(0x910*5*2, ROMEndTransfer, cpu);
        else*/
        ROMEndTransfer(cpu);
        printf("ROM transfer done. %08X %08X\n", ARM7Read32(0x03FFFFF8), ARM7Read32(0x03FFFFFC));
    }
}

u32 ROMReadData(u32 cpu)
{
    u32 ret = 0;

    switch (ROMCurCommand[0])
    {
    case 0x9F: ret = 0xFFFFFFFF; break;

    case 0x00:
        // TODO: feed an actual cart header!
        ret = 0;
        break;

    case 0x90:
        // chip ID
        ret = 0;
        break;
    }

    ROMReadPos += 4;
    if (ROMReadPos >= ROMReadSize)
        ROMEndTransfer(cpu);

    return ret;
}



void StartDiv()
{
    // TODO: division isn't instant!

    DivCnt &= ~0x2000;

    switch (DivCnt & 0x0003)
    {
    case 0x0000:
        {
            s32 num = (s32)DivNumerator[0];
            s32 den = (s32)DivDenominator[0];
            if (den == 0)
            {
                DivQuotient[0] = (num<0) ? 1:-1;
                DivQuotient[1] = (num<0) ? -1:1;
                *(s64*)&DivRemainder[0] = num;
            }
            else if (num == -0x80000000 && den == -1)
            {
                *(s64*)&DivQuotient[0] = 0x80000000;
            }
            else
            {
                *(s64*)&DivQuotient[0] = (s64)(num / den);
                *(s64*)&DivRemainder[0] = (s64)(num % den);
            }
        }
        break;

    case 0x0001:
    case 0x0003:
        {
            s64 num = *(s64*)&DivNumerator[0];
            s32 den = (s32)DivDenominator[0];
            if (den == 0)
            {
                *(s64*)&DivQuotient[0] = (num<0) ? 1:-1;
                *(s64*)&DivRemainder[0] = num;
            }
            else if (num == -0x8000000000000000 && den == -1)
            {
                *(s64*)&DivQuotient[0] = 0x8000000000000000;
            }
            else
            {
                *(s64*)&DivQuotient[0] = (s64)(num / den);
                *(s64*)&DivRemainder[0] = (s64)(num % den);
            }
        }
        break;

    case 0x0002:
        {
            s64 num = *(s64*)&DivNumerator[0];
            s64 den = *(s64*)&DivDenominator[0];
            if (den == 0)
            {
                *(s64*)&DivQuotient[0] = (num<0) ? 1:-1;
                *(s64*)&DivRemainder[0] = num;
            }
            else if (num == -0x8000000000000000 && den == -1)
            {
                *(s64*)&DivQuotient[0] = 0x8000000000000000;
            }
            else
            {
                *(s64*)&DivQuotient[0] = (s64)(num / den);
                *(s64*)&DivRemainder[0] = (s64)(num % den);
            }
        }
        break;
    }

    if ((DivDenominator[0] | DivDenominator[1]) == 0)
        DivCnt |= 0x2000;
}



void debug(u32 param)
{
    printf("ARM9 PC=%08X\n", ARM9->R[15]);
    printf("ARM7 PC=%08X\n", ARM7->R[15]);
}



u8 ARM9Read8(u32 addr)
{
    if ((addr & 0xFFFFF000) == 0xFFFF0000)
    {
        return *(u8*)&ARM9BIOS[addr & 0xFFF];
    }
    if (addr < ARM9ITCMSize)
    {
        return *(u8*)&ARM9ITCM[addr & 0x7FFF];
    }
    if (addr >= ARM9DTCMBase && addr < (ARM9DTCMBase + ARM9DTCMSize))
    {
        return *(u8*)&ARM9DTCM[(addr - ARM9DTCMBase) & 0x3FFF];
    }

    switch (addr & 0xFF000000)
    {
    case 0x02000000:
        return *(u8*)&MainRAM[addr & 0x3FFFFF];

    case 0x03000000:
        if (SWRAM_ARM9) return *(u8*)&SWRAM_ARM9[addr & SWRAM_ARM9Mask];
        else return 0;

    case 0x04000000:
        return ARM9IORead8(addr);

    case 0x05000000:
        return *(u8*)&GPU::Palette[addr & 0x7FF];

    case 0x06000000:
        {
            u32 chunk = (addr >> 14) & 0x7F;
            u8* vram = NULL;
            switch (addr & 0x00E00000)
            {
            case 0x00000000: vram = GPU::VRAM_ABG[chunk]; break;
            case 0x00200000: vram = GPU::VRAM_BBG[chunk]; break;
            case 0x00400000: vram = GPU::VRAM_AOBJ[chunk]; break;
            case 0x00600000: vram = GPU::VRAM_BOBJ[chunk]; break;
            case 0x00800000: vram = GPU::VRAM_LCD[chunk]; break;
            }
            if (vram)
                return *(u8*)&vram[addr & 0x3FFF];
        }
        return 0;

    case 0x07000000:
        return *(u8*)&GPU::OAM[addr & 0x7FF];

    case 0x08000000:
    case 0x09000000:
        return 0xFF;
    }

    printf("unknown arm9 read8 %08X\n", addr);
    return 0;
}

u16 ARM9Read16(u32 addr)
{
    if ((addr & 0xFFFFF000) == 0xFFFF0000)
    {
        return *(u16*)&ARM9BIOS[addr & 0xFFF];
    }
    if (addr < ARM9ITCMSize)
    {
        return *(u16*)&ARM9ITCM[addr & 0x7FFF];
    }
    if (addr >= ARM9DTCMBase && addr < (ARM9DTCMBase + ARM9DTCMSize))
    {
        return *(u16*)&ARM9DTCM[(addr - ARM9DTCMBase) & 0x3FFF];
    }

    switch (addr & 0xFF000000)
    {
    case 0x02000000:
        return *(u16*)&MainRAM[addr & 0x3FFFFF];

    case 0x03000000:
        if (SWRAM_ARM9) return *(u16*)&SWRAM_ARM9[addr & SWRAM_ARM9Mask];
        else return 0;

    case 0x04000000:
        return ARM9IORead16(addr);

    case 0x05000000:
        return *(u16*)&GPU::Palette[addr & 0x7FF];

    case 0x06000000:
        {
            u32 chunk = (addr >> 14) & 0x7F;
            u8* vram = NULL;
            switch (addr & 0x00E00000)
            {
            case 0x00000000: vram = GPU::VRAM_ABG[chunk]; break;
            case 0x00200000: vram = GPU::VRAM_BBG[chunk]; break;
            case 0x00400000: vram = GPU::VRAM_AOBJ[chunk]; break;
            case 0x00600000: vram = GPU::VRAM_BOBJ[chunk]; break;
            case 0x00800000: vram = GPU::VRAM_LCD[chunk]; break;
            }
            if (vram)
                return *(u16*)&vram[addr & 0x3FFF];
        }
        return 0;

    case 0x07000000:
        return *(u16*)&GPU::OAM[addr & 0x7FF];

    case 0x08000000:
    case 0x09000000:
        return 0xFFFF;
    }

    printf("unknown arm9 read16 %08X\n", addr);
    return 0;
}

u32 ARM9Read32(u32 addr)
{
    if ((addr & 0xFFFFF000) == 0xFFFF0000)
    {
        return *(u32*)&ARM9BIOS[addr & 0xFFF];
    }
    if (addr < ARM9ITCMSize)
    {
        return *(u32*)&ARM9ITCM[addr & 0x7FFF];
    }
    if (addr >= ARM9DTCMBase && addr < (ARM9DTCMBase + ARM9DTCMSize))
    {
        return *(u32*)&ARM9DTCM[(addr - ARM9DTCMBase) & 0x3FFF];
    }

    if (addr >= 0xFFFF1000)
    {
        printf("!!!!!!!!!!!!!\n");
        Halt();
        /*FILE* f = fopen("ram.bin", "wb");
        fwrite(MainRAM, 0x400000, 1, f);
        fclose(f);
        fopen("wram.bin", "wb");
        fwrite(ARM7WRAM, 0x10000, 1, f);
        fclose(f);
        fopen("swram.bin", "wb");
        fwrite(ARM7WRAM, 0x8000, 1, f);
        fclose(f);*/
    }

    switch (addr & 0xFF000000)
    {
    case 0x02000000:
        return *(u32*)&MainRAM[addr & 0x3FFFFF];

    case 0x03000000:
        if (SWRAM_ARM9) return *(u32*)&SWRAM_ARM9[addr & SWRAM_ARM9Mask];
        else return 0;

    case 0x04000000:
        return ARM9IORead32(addr);

    case 0x05000000:
        return *(u32*)&GPU::Palette[addr & 0x7FF];

    case 0x06000000:
        {
            u32 chunk = (addr >> 14) & 0x7F;
            u8* vram = NULL;
            switch (addr & 0x00E00000)
            {
            case 0x00000000: vram = GPU::VRAM_ABG[chunk]; break;
            case 0x00200000: vram = GPU::VRAM_BBG[chunk]; break;
            case 0x00400000: vram = GPU::VRAM_AOBJ[chunk]; break;
            case 0x00600000: vram = GPU::VRAM_BOBJ[chunk]; break;
            case 0x00800000: vram = GPU::VRAM_LCD[chunk]; break;
            }
            if (vram)
                return *(u32*)&vram[addr & 0x3FFF];
        }
        return 0;

    case 0x07000000:
        return *(u32*)&GPU::OAM[addr & 0x7FF];

    case 0x08000000:
    case 0x09000000:
        return 0xFFFFFFFF;
    }

    printf("unknown arm9 read32 %08X | %08X %08X %08X\n", addr, ARM9->R[15], ARM9->R[12], ARM9Read32(0x027FF820));
    return 0;
}

void ARM9Write8(u32 addr, u8 val)
{
    if (addr < ARM9ITCMSize)
    {
        *(u8*)&ARM9ITCM[addr & 0x7FFF] = val;
        return;
    }
    if (addr >= ARM9DTCMBase && addr < (ARM9DTCMBase + ARM9DTCMSize))
    {
        *(u8*)&ARM9DTCM[(addr - ARM9DTCMBase) & 0x3FFF] = val;
        return;
    }

    switch (addr & 0xFF000000)
    {
    case 0x02000000:
        *(u8*)&MainRAM[addr & 0x3FFFFF] = val;
        return;

    case 0x03000000:
        if (SWRAM_ARM9) *(u8*)&SWRAM_ARM9[addr & SWRAM_ARM9Mask] = val;
        return;

    case 0x04000000:
        ARM9IOWrite8(addr, val);
        return;

    case 0x05000000:
    case 0x06000000:
    case 0x07000000:
        return;
    }

    printf("unknown arm9 write8 %08X %02X\n", addr, val);
}

void ARM9Write16(u32 addr, u16 val)
{
    if (addr == ARM9->R[15]) printf("!!!!!!!!!!!!9999 %08X %04X\n", addr, val);

    if (addr < ARM9ITCMSize)
    {
        *(u16*)&ARM9ITCM[addr & 0x7FFF] = val;
        return;
    }
    if (addr >= ARM9DTCMBase && addr < (ARM9DTCMBase + ARM9DTCMSize))
    {
        *(u16*)&ARM9DTCM[(addr - ARM9DTCMBase) & 0x3FFF] = val;
        return;
    }

    switch (addr & 0xFF000000)
    {
    case 0x02000000:
        *(u16*)&MainRAM[addr & 0x3FFFFF] = val;
        return;

    case 0x03000000:
        if (SWRAM_ARM9) *(u16*)&SWRAM_ARM9[addr & SWRAM_ARM9Mask] = val;
        return;

    case 0x04000000:
        ARM9IOWrite16(addr, val);
        return;

    case 0x05000000:
        *(u16*)&GPU::Palette[addr & 0x7FF] = val;
        return;

    case 0x06000000:
        {
            u32 chunk = (addr >> 14) & 0x7F;
            u8* vram = NULL;
            switch (addr & 0x00E00000)
            {
            case 0x00000000: vram = GPU::VRAM_ABG[chunk]; break;
            case 0x00200000: vram = GPU::VRAM_BBG[chunk]; break;
            case 0x00400000: vram = GPU::VRAM_AOBJ[chunk]; break;
            case 0x00600000: vram = GPU::VRAM_BOBJ[chunk]; break;
            case 0x00800000: vram = GPU::VRAM_LCD[chunk]; break;
            }
            if (vram)
                *(u16*)&vram[addr & 0x3FFF] = val;
        }
        return;

    case 0x07000000:
        *(u16*)&GPU::OAM[addr & 0x7FF] = val;
        return;
    }

    printf("unknown arm9 write16 %08X %04X\n", addr, val);
}

void ARM9Write32(u32 addr, u32 val)
{
    if (addr == ARM9->R[15]) printf("!!!!!!!!!!!!9999 %08X %08X\n", addr, val);

    if (addr < ARM9ITCMSize)
    {
        *(u32*)&ARM9ITCM[addr & 0x7FFF] = val;
        return;
    }
    if (addr >= ARM9DTCMBase && addr < (ARM9DTCMBase + ARM9DTCMSize))
    {
        *(u32*)&ARM9DTCM[(addr - ARM9DTCMBase) & 0x3FFF] = val;
        return;
    }

    switch (addr & 0xFF000000)
    {
    case 0x02000000:
        *(u32*)&MainRAM[addr & 0x3FFFFF] = val;
        return;

    case 0x03000000:
        if (SWRAM_ARM9) *(u32*)&SWRAM_ARM9[addr & SWRAM_ARM9Mask] = val;
        return;

    case 0x04000000:
        ARM9IOWrite32(addr, val);
        return;

    case 0x05000000:
        *(u32*)&GPU::Palette[addr & 0x7FF] = val;
        return;

    case 0x06000000:
        {
            u32 chunk = (addr >> 14) & 0x7F;
            u8* vram = NULL;
            switch (addr & 0x00E00000)
            {
            case 0x00000000: vram = GPU::VRAM_ABG[chunk]; break;
            case 0x00200000: vram = GPU::VRAM_BBG[chunk]; break;
            case 0x00400000: vram = GPU::VRAM_AOBJ[chunk]; break;
            case 0x00600000: vram = GPU::VRAM_BOBJ[chunk]; break;
            case 0x00800000: vram = GPU::VRAM_LCD[chunk]; break;
            }
            if (vram)
                *(u32*)&vram[addr & 0x3FFF] = val;
        }
        return;

    case 0x07000000:
        *(u32*)&GPU::OAM[addr & 0x7FF] = val;
        return;
    }

    printf("unknown arm9 write32 %08X %08X | %08X\n", addr, val, ARM9->R[15]);
}



u8 ARM7Read8(u32 addr)
{
    if (addr < 0x00004000)
    {
        if (ARM7->R[15] > 0x4000) printf("BAD BIOS READ8 %08X FROM %08X\n", addr, ARM7->R[15]);
        return *(u8*)&ARM7BIOS[addr];
    }

    switch (addr & 0xFF800000)
    {
    case 0x02000000:
        return *(u8*)&MainRAM[addr & 0x3FFFFF];

    case 0x03000000:
        if (SWRAM_ARM7) return *(u8*)&SWRAM_ARM7[addr & SWRAM_ARM7Mask];
        else return *(u8*)&ARM7WRAM[addr & 0xFFFF];

    case 0x03800000:
        return *(u8*)&ARM7WRAM[addr & 0xFFFF];

    case 0x04000000:
        return ARM7IORead8(addr);

    case 0x06000000:
    case 0x06800000:
        {
            u32 chunk = (addr >> 17) & 0x1;
            u8* vram = GPU::VRAM_ARM7[chunk];
            if (vram)
                return *(u8*)&vram[addr & 0x3FFF];
        }
        return 0;
    }

    printf("unknown arm7 read8 %08X %08X %08X/%08X\n", addr, ARM7->R[15], ARM7->R[0], ARM7->R[1]);
    return 0;
}

u16 ARM7Read16(u32 addr)
{
    if (addr < 0x00004000)
    {
        if (ARM7->R[15] > 0x4000) printf("BAD BIOS READ16 %08X FROM %08X\n", addr, ARM7->R[15]);
        return *(u16*)&ARM7BIOS[addr];
    }

    switch (addr & 0xFF800000)
    {
    case 0x02000000:
        return *(u16*)&MainRAM[addr & 0x3FFFFF];

    case 0x03000000:
        if (SWRAM_ARM7) return *(u16*)&SWRAM_ARM7[addr & SWRAM_ARM7Mask];
        else return *(u16*)&ARM7WRAM[addr & 0xFFFF];

    case 0x03800000:
        return *(u16*)&ARM7WRAM[addr & 0xFFFF];

    case 0x04000000:
        return ARM7IORead16(addr);

    case 0x04800000:
        return Wifi::Read(addr);

    case 0x06000000:
    case 0x06800000:
        {
            u32 chunk = (addr >> 17) & 0x1;
            u8* vram = GPU::VRAM_ARM7[chunk];
            if (vram)
                return *(u16*)&vram[addr & 0x3FFF];
        }
        return 0;
    }

    printf("unknown arm7 read16 %08X %08X\n", addr, ARM7->R[15]);
    return 0;
}

u32 ARM7Read32(u32 addr)
{
    if (addr < 0x00004000)
    {
        if (ARM7->R[15] > 0x4000) {
                printf("BAD BIOS READ32 %08X FROM %08X | %08X %08X\n", addr, ARM7->R[15], ARM7Read32(0x03807758+12), ARM7Read32(0x03807758+4));
                Halt();
        return 0xFFFFFFFF;
        }
        //if (addr < 0x1204 && ARM7->R[15] >= 0x1204) printf("BAD BIOS READ32 %08X FROM %08X\n", addr, ARM7->R[15]);
        return *(u32*)&ARM7BIOS[addr];
    }

    switch (addr & 0xFF800000)
    {
    case 0x02000000:
        return *(u32*)&MainRAM[addr & 0x3FFFFF];

    case 0x03000000:
        if (SWRAM_ARM7) return *(u32*)&SWRAM_ARM7[addr & SWRAM_ARM7Mask];
        else return *(u32*)&ARM7WRAM[addr & 0xFFFF];

    case 0x03800000:
        return *(u32*)&ARM7WRAM[addr & 0xFFFF];

    case 0x04000000:
        return ARM7IORead32(addr);

    case 0x06000000:
    case 0x06800000:
        {
            u32 chunk = (addr >> 17) & 0x1;
            u8* vram = GPU::VRAM_ARM7[chunk];
            if (vram)
                return *(u32*)&vram[addr & 0x3FFF];
        }
        return 0;
    }

    printf("unknown arm7 read32 %08X | %08X\n", addr, ARM7->R[15]);
    return 0;
}

void ARM7Write8(u32 addr, u8 val)
{
    if (addr==0x3807764) printf("DERP! %02X %08X\n", val, ARM7->R[15]);
    if (addr==0x27FFCE4) printf("FIRMWARE STATUS8 %04X %08X\n", val, ARM7->R[15]);
    switch (addr & 0xFF800000)
    {
    case 0x02000000:
        *(u8*)&MainRAM[addr & 0x3FFFFF] = val;
        return;

    case 0x03000000:
        if (SWRAM_ARM7) *(u8*)&SWRAM_ARM7[addr & SWRAM_ARM7Mask] = val;
        else *(u8*)&ARM7WRAM[addr & 0xFFFF] = val;
        return;

    case 0x03800000:
        *(u8*)&ARM7WRAM[addr & 0xFFFF] = val;
        return;

    case 0x04000000:
        ARM7IOWrite8(addr, val);
        return;

    case 0x06000000:
    case 0x06800000:
        {
            u32 chunk = (addr >> 17) & 0x1;
            u8* vram = GPU::VRAM_ARM7[chunk];
            if (vram)
                *(u8*)&vram[addr & 0x3FFF] = val;
        }
        return;
    }

    printf("unknown arm7 write8 %08X %02X | %08X | %08X %08X %08X %08X\n", addr, val, ARM7->R[15], IME[1], IE[1], ARM7->R[0], ARM7->R[1]);
}

void ARM7Write16(u32 addr, u16 val)
{
    if (addr == ARM7->R[15]) printf("!!!!!!!!!!!!7777 %08X %04X\n", addr, val);
    if (addr==0x3807764) printf("DERP! %04X %08X\n", val, ARM7->R[15]);
    if (addr==0x27FF816) printf("RTC STATUS %04X %08X\n", val, ARM7->R[15]);
    if (addr==0x27FFCE4) printf("FIRMWARE STATUS %04X %08X\n", val, ARM7->R[15]);
    switch (addr & 0xFF800000)
    {
    case 0x02000000:
        *(u16*)&MainRAM[addr & 0x3FFFFF] = val;
        return;

    case 0x03000000:
        if (SWRAM_ARM7) *(u16*)&SWRAM_ARM7[addr & SWRAM_ARM7Mask] = val;
        else *(u16*)&ARM7WRAM[addr & 0xFFFF] = val;
        return;

    case 0x03800000:
        *(u16*)&ARM7WRAM[addr & 0xFFFF] = val;
        return;

    case 0x04000000:
        ARM7IOWrite16(addr, val);
        return;

    case 0x04800000:
        Wifi::Write(addr, val);
        return;

    case 0x06000000:
    case 0x06800000:
        {
            u32 chunk = (addr >> 17) & 0x1;
            u8* vram = GPU::VRAM_ARM7[chunk];
            if (vram)
                *(u16*)&vram[addr & 0x3FFF] = val;
        }
        return;
    }

    printf("unknown arm7 write16 %08X %04X | %08X\n", addr, val, ARM7->R[15]);
}

void ARM7Write32(u32 addr, u32 val)
{
    if (addr == ARM7->R[15]) printf("!!!!!!!!!!!!7777 %08X %08X\n", addr, val);
if (addr==0x27FFCE4) printf("FIRMWARE STATUS32 %08X %08X\n", val, ARM7->R[15]);
    switch (addr & 0xFF800000)
    {
    case 0x02000000:
        *(u32*)&MainRAM[addr & 0x3FFFFF] = val;
        return;

    case 0x03000000:
        if (SWRAM_ARM7) *(u32*)&SWRAM_ARM7[addr & SWRAM_ARM7Mask] = val;
        else *(u32*)&ARM7WRAM[addr & 0xFFFF] = val;
        return;

    case 0x03800000:
        *(u32*)&ARM7WRAM[addr & 0xFFFF] = val;
        return;

    case 0x04000000:
        ARM7IOWrite32(addr, val);
        return;

    case 0x06000000:
    case 0x06800000:
        {
            u32 chunk = (addr >> 17) & 0x1;
            u8* vram = GPU::VRAM_ARM7[chunk];
            if (vram)
                *(u32*)&vram[addr & 0x3FFF] = val;
        }
        return;
    }

    printf("unknown arm7 write32 %08X %08X | %08X %08X\n", addr, val, ARM7->R[15], ARM7->CurInstr);
}




u8 ARM9IORead8(u32 addr)
{
    switch (addr)
    {
    case 0x04000208: return IME[0];

    case 0x04000240: return GPU::VRAMCNT[0];
    case 0x04000241: return GPU::VRAMCNT[1];
    case 0x04000242: return GPU::VRAMCNT[2];
    case 0x04000243: return GPU::VRAMCNT[3];
    case 0x04000244: return GPU::VRAMCNT[4];
    case 0x04000245: return GPU::VRAMCNT[5];
    case 0x04000246: return GPU::VRAMCNT[6];
    case 0x04000247: return WRAMCnt;
    case 0x04000248: return GPU::VRAMCNT[7];
    case 0x04000249: return GPU::VRAMCNT[8];

    case 0x04000300: return PostFlag9;
    }

    if (addr >= 0x04000000 && addr < 0x04000060)
    {
        return GPU::GPU2D_A->Read8(addr);
    }
    if (addr >= 0x04001000 && addr < 0x04001060)
    {
        return GPU::GPU2D_B->Read8(addr);
    }

    printf("unknown ARM9 IO read8 %08X\n", addr);
    return 0;
}

u16 ARM9IORead16(u32 addr)
{
    switch (addr)
    {
    case 0x04000004: return GPU::DispStat[0];
    case 0x04000006: return GPU::VCount;

    case 0x040000E0: return ((u16*)DMA9Fill)[0];
    case 0x040000E2: return ((u16*)DMA9Fill)[1];
    case 0x040000E4: return ((u16*)DMA9Fill)[2];
    case 0x040000E6: return ((u16*)DMA9Fill)[3];
    case 0x040000E8: return ((u16*)DMA9Fill)[4];
    case 0x040000EA: return ((u16*)DMA9Fill)[5];
    case 0x040000EC: return ((u16*)DMA9Fill)[6];
    case 0x040000EE: return ((u16*)DMA9Fill)[7];

    case 0x04000100: return Timers[0].Counter;
    case 0x04000102: return Timers[0].Control;
    case 0x04000104: return Timers[1].Counter;
    case 0x04000106: return Timers[1].Control;
    case 0x04000108: return Timers[2].Counter;
    case 0x0400010A: return Timers[2].Control;
    case 0x0400010C: return Timers[3].Counter;
    case 0x0400010E: return Timers[3].Control;

    case 0x04000130: return KeyInput & 0xFFFF;

    case 0x04000180: return IPCSync9;
    case 0x04000184:
        {
            u16 val = IPCFIFOCnt9;
            if (IPCFIFO9->IsEmpty())     val |= 0x0001;
            else if (IPCFIFO9->IsFull()) val |= 0x0002;
            if (IPCFIFO7->IsEmpty())     val |= 0x0100;
            else if (IPCFIFO7->IsFull()) val |= 0x0200;
            return val;
        }

    case 0x04000204: return 0;//0xFFFF;

    case 0x04000208: return IME[0];

    case 0x04000280: return DivCnt;

    case 0x04000300: return PostFlag9;
    case 0x04000304: return PowerControl9;
    }

    if (addr >= 0x04000000 && addr < 0x04000060)
    {
        return GPU::GPU2D_A->Read16(addr);
    }
    if (addr >= 0x04001000 && addr < 0x04001060)
    {
        return GPU::GPU2D_B->Read16(addr);
    }

    printf("unknown ARM9 IO read16 %08X %08X\n", addr, ARM9->R[15]);
    return 0;
}

u32 ARM9IORead32(u32 addr)
{
    switch (addr)
    {
    case 0x04000004: return GPU::DispStat[0] | (GPU::VCount << 16);

    case 0x040000B0: return DMAs[0]->SrcAddr;
    case 0x040000B4: return DMAs[0]->DstAddr;
    case 0x040000B8: return DMAs[0]->Cnt;
    case 0x040000BC: return DMAs[1]->SrcAddr;
    case 0x040000C0: return DMAs[1]->DstAddr;
    case 0x040000C4: return DMAs[1]->Cnt;
    case 0x040000C8: return DMAs[2]->SrcAddr;
    case 0x040000CC: return DMAs[2]->DstAddr;
    case 0x040000D0: return DMAs[2]->Cnt;
    case 0x040000D4: return DMAs[3]->SrcAddr;
    case 0x040000D8: return DMAs[3]->DstAddr;
    case 0x040000DC: return DMAs[3]->Cnt;

    case 0x040000E0: return DMA9Fill[0];
    case 0x040000E4: return DMA9Fill[1];
    case 0x040000E8: return DMA9Fill[2];
    case 0x040000EC: return DMA9Fill[3];

    case 0x04000100: return Timers[0].Counter | (Timers[0].Control << 16);
    case 0x04000104: return Timers[1].Counter | (Timers[1].Control << 16);
    case 0x04000108: return Timers[2].Counter | (Timers[2].Control << 16);
    case 0x0400010C: return Timers[3].Counter | (Timers[3].Control << 16);

    case 0x04000208: return IME[0];
    case 0x04000210: return IE[0];
    case 0x04000214: return IF[0];

    case 0x04000290: return DivNumerator[0];
    case 0x04000294: return DivNumerator[1];
    case 0x04000298: return DivDenominator[0];
    case 0x0400029C: return DivDenominator[1];
    case 0x040002A0: return DivQuotient[0];
    case 0x040002A4: return DivQuotient[1];
    case 0x040002A8: return DivRemainder[0];
    case 0x040002AC: return DivRemainder[1];

    case 0x04100000:
        if (IPCFIFOCnt9 & 0x8000)
        {
            u32 ret;
            if (IPCFIFO7->IsEmpty())
            {
                IPCFIFOCnt9 |= 0x4000;
                ret = IPCFIFO7->Peek();
            }
            else
            {
                ret = IPCFIFO7->Read();

                if (IPCFIFO7->IsEmpty() && (IPCFIFOCnt7 & 0x0004))
                    TriggerIRQ(1, IRQ_IPCSendDone);
            }
            return ret;
        }
        else
            return IPCFIFO7->Peek();
    }

    if (addr >= 0x04000000 && addr < 0x04000060)
    {
        return GPU::GPU2D_A->Read32(addr);
    }
    if (addr >= 0x04001000 && addr < 0x04001060)
    {
        return GPU::GPU2D_B->Read32(addr);
    }

    printf("unknown ARM9 IO read32 %08X\n", addr);
    return 0;
}

void ARM9IOWrite8(u32 addr, u8 val)
{
    switch (addr)
    {
    case 0x040001A0:
        ROMSPIControl &= 0xFF00;
        ROMSPIControl |= val;
        return;
    case 0x040001A1:
        ROMSPIControl &= 0x00FF;
        ROMSPIControl |= (val << 8);
        return;

    case 0x04000208: IME[0] = val & 0x1; return;

    case 0x04000240: GPU::MapVRAM_AB(0, val); return;
    case 0x04000241: GPU::MapVRAM_AB(1, val); return;
    case 0x04000242: GPU::MapVRAM_CD(2, val); return;
    case 0x04000243: GPU::MapVRAM_CD(3, val); return;
    case 0x04000244: GPU::MapVRAM_E(4, val); return;
    case 0x04000245: GPU::MapVRAM_FG(5, val); return;
    case 0x04000246: GPU::MapVRAM_FG(6, val); return;
    case 0x04000247: MapSharedWRAM(val); return;
    case 0x04000248: GPU::MapVRAM_H(7, val); return;
    case 0x04000249: GPU::MapVRAM_I(8, val); return;

    case 0x04000300:
        if (PostFlag9 & 0x01) val |= 0x01;
        PostFlag9 = val & 0x03;
        return;
    }

    if (addr >= 0x04000000 && addr < 0x04000060)
    {
        GPU::GPU2D_A->Write8(addr, val);
        return;
    }
    if (addr >= 0x04001000 && addr < 0x04001060)
    {
        GPU::GPU2D_B->Write8(addr, val);
        return;
    }

    printf("unknown ARM9 IO write8 %08X %02X\n", addr, val);
}

void ARM9IOWrite16(u32 addr, u16 val)
{
    switch (addr)
    {
    case 0x04000004: GPU::SetDispStat(0, val); return;

    case 0x04000100: Timers[0].Reload = val; return;
    case 0x04000102: TimerStart(0, val); return;
    case 0x04000104: Timers[1].Reload = val; return;
    case 0x04000106: TimerStart(1, val); return;
    case 0x04000108: Timers[2].Reload = val; return;
    case 0x0400010A: TimerStart(2, val); return;
    case 0x0400010C: Timers[3].Reload = val; return;
    case 0x0400010E: TimerStart(3, val); return;

    case 0x04000180:
        IPCSync7 &= 0xFFF0;
        IPCSync7 |= ((val & 0x0F00) >> 8);
        IPCSync9 &= 0xB0FF;
        IPCSync9 |= (val & 0x4F00);
        if ((val & 0x2000) && (IPCSync7 & 0x4000))
        {
            TriggerIRQ(1, IRQ_IPCSync);
        }
        CompensateARM7();
        return;

    case 0x04000184:
        if (val & 0x0008)
            IPCFIFO9->Clear();
        if ((val & 0x0004) && (!(IPCFIFOCnt9 & 0x0004)) && IPCFIFO9->IsEmpty())
            TriggerIRQ(0, IRQ_IPCSendDone);
        if ((val & 0x0400) && (!(IPCFIFOCnt9 & 0x0400)) && (!IPCFIFO7->IsEmpty()))
            TriggerIRQ(0, IRQ_IPCRecv);
        if (val & 0x4000)
            IPCFIFOCnt9 &= ~0x4000;
        IPCFIFOCnt9 = val & 0x8404;
        return;

    case 0x040001A0:
        ROMSPIControl = val;
        return;

    case 0x04000208: IME[0] = val & 0x1; return;

    case 0x04000240:
        GPU::MapVRAM_AB(0, val & 0xFF);
        GPU::MapVRAM_AB(1, val >> 8);
        return;
    case 0x04000242:
        GPU::MapVRAM_CD(2, val & 0xFF);
        GPU::MapVRAM_CD(3, val >> 8);
        return;
    case 0x04000244:
        GPU::MapVRAM_E(4, val & 0xFF);
        GPU::MapVRAM_FG(5, val >> 8);
        return;
    case 0x04000246:
        GPU::MapVRAM_FG(6, val & 0xFF);
        MapSharedWRAM(val >> 8);
        return;
    case 0x04000248:
        GPU::MapVRAM_H(7, val & 0xFF);
        GPU::MapVRAM_I(8, val >> 8);
        return;

    case 0x04000280: DivCnt = val; StartDiv(); return;

    case 0x04000300:
        if (PostFlag9 & 0x01) val |= 0x01;
        PostFlag9 = val & 0x03;
        return;

    case 0x04000304: PowerControl9 = val; return;
    }

    if (addr >= 0x04000000 && addr < 0x04000060)
    {
        GPU::GPU2D_A->Write16(addr, val);
        return;
    }
    if (addr >= 0x04001000 && addr < 0x04001060)
    {
        GPU::GPU2D_B->Write16(addr, val);
        return;
    }

    printf("unknown ARM9 IO write16 %08X %04X\n", addr, val);
}

void ARM9IOWrite32(u32 addr, u32 val)
{
    switch (addr)
    {
    case 0x040000B0: DMAs[0]->SrcAddr = val; return;
    case 0x040000B4: DMAs[0]->DstAddr = val; return;
    case 0x040000B8: DMAs[0]->WriteCnt(val); return;
    case 0x040000BC: DMAs[1]->SrcAddr = val; return;
    case 0x040000C0: DMAs[1]->DstAddr = val; return;
    case 0x040000C4: DMAs[1]->WriteCnt(val); return;
    case 0x040000C8: DMAs[2]->SrcAddr = val; return;
    case 0x040000CC: DMAs[2]->DstAddr = val; return;
    case 0x040000D0: DMAs[2]->WriteCnt(val); return;
    case 0x040000D4: DMAs[3]->SrcAddr = val; return;
    case 0x040000D8: DMAs[3]->DstAddr = val; return;
    case 0x040000DC: DMAs[3]->WriteCnt(val); return;

    case 0x040000E0: DMA9Fill[0] = val; return;
    case 0x040000E4: DMA9Fill[1] = val; return;
    case 0x040000E8: DMA9Fill[2] = val; return;
    case 0x040000EC: DMA9Fill[3] = val; return;

    case 0x04000100:
        Timers[0].Reload = val & 0xFFFF;
        TimerStart(0, val>>16);
        return;
    case 0x04000104:
        Timers[1].Reload = val & 0xFFFF;
        TimerStart(1, val>>16);
        return;
    case 0x04000108:
        Timers[2].Reload = val & 0xFFFF;
        TimerStart(2, val>>16);
        return;
    case 0x0400010C:
        Timers[3].Reload = val & 0xFFFF;
        TimerStart(3, val>>16);
        return;

    case 0x04000188:
        if (IPCFIFOCnt9 & 0x8000)
        {
            if (IPCFIFO9->IsFull())
                IPCFIFOCnt9 |= 0x4000;
            else
            {
                bool wasempty = IPCFIFO9->IsEmpty();
                IPCFIFO9->Write(val);
                if ((IPCFIFOCnt7 & 0x0400) && wasempty)
                    TriggerIRQ(1, IRQ_IPCRecv);
            }
        }
        return;

    case 0x040001A0:
        ROMSPIControl = val & 0xFFFF;
        // TODO: SPI shit
        return;
    case 0x040001A4:
        val &= ~0x00800000;
        ROMControl = val;
        if (val & 0x80000000) ROMStartTransfer(0);
        return;

    case 0x04000208: IME[0] = val & 0x1; return;
    case 0x04000210: IE[0] = val; if (val&~0x000F0F7D)printf("unusual IRQ %08X\n",val);return;
    case 0x04000214: IF[0] &= ~val; return;

    case 0x04000240:
        GPU::MapVRAM_AB(0, val & 0xFF);
        GPU::MapVRAM_AB(1, (val >> 8) & 0xFF);
        GPU::MapVRAM_CD(2, (val >> 16) & 0xFF);
        GPU::MapVRAM_CD(3, val >> 24);
        return;
    case 0x04000244:
        GPU::MapVRAM_E(4, val & 0xFF);
        GPU::MapVRAM_FG(5, (val >> 8) & 0xFF);
        GPU::MapVRAM_FG(6, (val >> 16) & 0xFF);
        MapSharedWRAM(val >> 24);
        return;
    case 0x04000248:
        GPU::MapVRAM_H(7, val & 0xFF);
        GPU::MapVRAM_I(8, (val >> 8) & 0xFF);
        return;

    case 0x04000290: DivNumerator[0] = val; StartDiv(); return;
    case 0x04000294: DivNumerator[1] = val; StartDiv(); return;
    case 0x04000298: DivDenominator[0] = val; StartDiv(); return;
    case 0x0400029C: DivDenominator[1] = val; StartDiv(); return;
    }

    if (addr >= 0x04000000 && addr < 0x04000060)
    {
        GPU::GPU2D_A->Write32(addr, val);
        return;
    }
    if (addr >= 0x04001000 && addr < 0x04001060)
    {
        GPU::GPU2D_B->Write32(addr, val);
        return;
    }

    printf("unknown ARM9 IO write32 %08X %08X\n", addr, val);
}


u8 ARM7IORead8(u32 addr)
{
    switch (addr)
    {
    case 0x04000138: return RTC::Read() & 0xFF;

    case 0x040001C2: return SPI::ReadData();

    case 0x04000208: return IME[1];

    case 0x04000240: return GPU::VRAMSTAT;
    case 0x04000241: return WRAMCnt;

    case 0x04000300: return PostFlag7;

    //case 0x04000403:
        //Halt();
        //return 0;
    }

    if (addr >= 0x04000400 && addr < 0x04000520)
    {
        // sound I/O
        return 0;
    }

    printf("unknown ARM7 IO read8 %08X\n", addr);
    return 0;
}

u16 ARM7IORead16(u32 addr)
{
    switch (addr)
    {
    case 0x04000004: return GPU::DispStat[1];
    case 0x04000006: return GPU::VCount;

    case 0x04000100: return Timers[4].Counter;
    case 0x04000102: return Timers[4].Control;
    case 0x04000104: return Timers[5].Counter;
    case 0x04000106: return Timers[5].Control;
    case 0x04000108: return Timers[6].Counter;
    case 0x0400010A: return Timers[6].Control;
    case 0x0400010C: return Timers[7].Counter;
    case 0x0400010E: return Timers[7].Control;

    case 0x04000130: return KeyInput & 0xFFFF;
    case 0x04000136: return KeyInput >> 16;

    case 0x04000134: return 0x8000;
    case 0x04000138: return RTC::Read();

    case 0x04000180: return IPCSync7;
    case 0x04000184:
        {
            u16 val = IPCFIFOCnt7;
            if (IPCFIFO7->IsEmpty())     val |= 0x0001;
            else if (IPCFIFO7->IsFull()) val |= 0x0002;
            if (IPCFIFO9->IsEmpty())     val |= 0x0100;
            else if (IPCFIFO9->IsFull()) val |= 0x0200;
            return val;
        }

    case 0x040001C0: return SPI::ReadCnt();
    case 0x040001C2: return SPI::ReadData();

    case 0x04000208: return IME[1];

    case 0x04000300: return PostFlag7;
    case 0x04000304: return PowerControl7;

    case 0x04000504: return _soundbias;
    }

    if (addr >= 0x04000400 && addr < 0x04000520)
    {
        // sound I/O
        return 0;
    }

    printf("unknown ARM7 IO read16 %08X %08X\n", addr, ARM9->R[15]);
    return 0;
}

u32 ARM7IORead32(u32 addr)
{
    switch (addr)
    {
    case 0x04000004: return GPU::DispStat[1] | (GPU::VCount << 16);

    case 0x040000B0: return DMAs[4]->SrcAddr;
    case 0x040000B4: return DMAs[4]->DstAddr;
    case 0x040000B8: return DMAs[4]->Cnt;
    case 0x040000BC: return DMAs[5]->SrcAddr;
    case 0x040000C0: return DMAs[5]->DstAddr;
    case 0x040000C4: return DMAs[5]->Cnt;
    case 0x040000C8: return DMAs[6]->SrcAddr;
    case 0x040000CC: return DMAs[6]->DstAddr;
    case 0x040000D0: return DMAs[6]->Cnt;
    case 0x040000D4: return DMAs[7]->SrcAddr;
    case 0x040000D8: return DMAs[7]->DstAddr;
    case 0x040000DC: return DMAs[7]->Cnt;

    case 0x04000100: return Timers[4].Counter | (Timers[4].Control << 16);
    case 0x04000104: return Timers[5].Counter | (Timers[5].Control << 16);
    case 0x04000108: return Timers[6].Counter | (Timers[6].Control << 16);
    case 0x0400010C: return Timers[7].Counter | (Timers[7].Control << 16);

    case 0x040001A4:
        return ROMControl;

    case 0x040001C0:
        return SPI::ReadCnt() | (SPI::ReadData() << 16);

    case 0x04000208: return IME[1];
    case 0x04000210: return IE[1];
    case 0x04000214: return IF[1];

    case 0x04100000:
        if (IPCFIFOCnt7 & 0x8000)
        {
            u32 ret;
            if (IPCFIFO9->IsEmpty())
            {
                IPCFIFOCnt7 |= 0x4000;
                ret = IPCFIFO9->Peek();
            }
            else
            {
                ret = IPCFIFO9->Read();

                if (IPCFIFO9->IsEmpty() && (IPCFIFOCnt9 & 0x0004))
                    TriggerIRQ(0, IRQ_IPCSendDone);
            }
            return ret;
        }
        else
            return IPCFIFO9->Peek();

    case 0x04100010: return ROMReadData(1);
    }

    if (addr >= 0x04000400 && addr < 0x04000520)
    {
        // sound I/O
        return 0;
    }

    printf("unknown ARM7 IO read32 %08X\n", addr);
    return 0;
}

void ARM7IOWrite8(u32 addr, u8 val)
{
    switch (addr)
    {
    case 0x04000138: RTC::Write(val, true); return;

    case 0x040001A0:
        ROMSPIControl &= 0xFF00;
        ROMSPIControl |= val;
        return;
    case 0x040001A1:
        ROMSPIControl &= 0x00FF;
        ROMSPIControl |= (val << 8);
        return;

    case 0x040001A8: ROMCommand[0] = val; return;
    case 0x040001A9: ROMCommand[1] = val; return;
    case 0x040001AA: ROMCommand[2] = val; return;
    case 0x040001AB: ROMCommand[3] = val; return;
    case 0x040001AC: ROMCommand[4] = val; return;
    case 0x040001AD: ROMCommand[5] = val; return;
    case 0x040001AE: ROMCommand[6] = val; return;
    case 0x040001AF: ROMCommand[7] = val; return;

    case 0x040001C2:
        SPI::WriteData(val);
        return;

    case 0x04000208: IME[1] = val & 0x1; return;

    case 0x04000300:
        if (ARM7->R[15] >= 0x4000)
            return;
        if (!(PostFlag7 & 0x01))
            PostFlag7 = val & 0x01;
        return;

    case 0x04000301:
        if (val == 0x80) ARM7->Halt(1);
        return;
    }

    if (addr >= 0x04000400 && addr < 0x04000520)
    {
        // sound I/O
        return;
    }

    printf("unknown ARM7 IO write8 %08X %02X\n", addr, val);
}

void ARM7IOWrite16(u32 addr, u16 val)
{
    switch (addr)
    {
    case 0x04000004: GPU::SetDispStat(1, val); return;

    case 0x04000100: Timers[4].Reload = val; return;
    case 0x04000102: TimerStart(4, val); return;
    case 0x04000104: Timers[5].Reload = val; return;
    case 0x04000106: TimerStart(5, val); return;
    case 0x04000108: Timers[6].Reload = val; return;
    case 0x0400010A: TimerStart(6, val); return;
    case 0x0400010C: Timers[7].Reload = val; return;
    case 0x0400010E: TimerStart(7, val); return;

    case 0x04000134: return;printf("set debug port %04X %08X\n", val, ARM7Read32(ARM7->R[13]+4)); return;

    case 0x04000138: RTC::Write(val, false); return;

    case 0x04000180:
        IPCSync9 &= 0xFFF0;
        IPCSync9 |= ((val & 0x0F00) >> 8);
        IPCSync7 &= 0xB0FF;
        IPCSync7 |= (val & 0x4F00);
        if ((val & 0x2000) && (IPCSync9 & 0x4000))
        {
            TriggerIRQ(0, IRQ_IPCSync);
        }
        return;

    case 0x04000184:
        if (val & 0x0008)
            IPCFIFO7->Clear();
        if ((val & 0x0004) && (!(IPCFIFOCnt7 & 0x0004)) && IPCFIFO7->IsEmpty())
            TriggerIRQ(1, IRQ_IPCSendDone);
        if ((val & 0x0400) && (!(IPCFIFOCnt7 & 0x0400)) && (!IPCFIFO9->IsEmpty()))
            TriggerIRQ(1, IRQ_IPCRecv);
        if (val & 0x4000)
            IPCFIFOCnt7 &= ~0x4000;
        IPCFIFOCnt7 = val & 0x8404;
        return;

    case 0x040001A0:
        ROMSPIControl = val;
        return;

    case 0x040001C0:
        SPI::WriteCnt(val);
        return;

    case 0x040001C2:
        SPI::WriteData(val & 0xFF);
        return;

    case 0x04000208: IME[1] = val & 0x1; return;

    case 0x04000300:
        if (ARM7->R[15] >= 0x4000)
            return;
        if (!(PostFlag7 & 0x01))
            PostFlag7 = val & 0x01;
        return;

    case 0x04000304: PowerControl7 = val; return;

    case 0x04000504:
        _soundbias = val & 0x3FF;
        return;
    }

    if (addr >= 0x04000400 && addr < 0x04000520)
    {
        // sound I/O
        return;
    }

    printf("unknown ARM7 IO write16 %08X %04X\n", addr, val);
}

void ARM7IOWrite32(u32 addr, u32 val)
{
    switch (addr)
    {
    case 0x040000B0: DMAs[4]->SrcAddr = val; return;
    case 0x040000B4: DMAs[4]->DstAddr = val; return;
    case 0x040000B8: DMAs[4]->WriteCnt(val); return;
    case 0x040000BC: DMAs[5]->SrcAddr = val; return;
    case 0x040000C0: DMAs[5]->DstAddr = val; return;
    case 0x040000C4: DMAs[5]->WriteCnt(val); return;
    case 0x040000C8: DMAs[6]->SrcAddr = val; return;
    case 0x040000CC: DMAs[6]->DstAddr = val; return;
    case 0x040000D0: DMAs[6]->WriteCnt(val); return;
    case 0x040000D4: DMAs[7]->SrcAddr = val; return;
    case 0x040000D8: DMAs[7]->DstAddr = val; return;
    case 0x040000DC: DMAs[7]->WriteCnt(val); return;

    case 0x04000100:
        Timers[4].Reload = val & 0xFFFF;
        TimerStart(4, val>>16);
        return;
    case 0x04000104:
        Timers[5].Reload = val & 0xFFFF;
        TimerStart(5, val>>16);
        return;
    case 0x04000108:
        Timers[6].Reload = val & 0xFFFF;
        TimerStart(6, val>>16);
        return;
    case 0x0400010C:
        Timers[7].Reload = val & 0xFFFF;
        TimerStart(7, val>>16);
        return;

    case 0x04000188:
        if (IPCFIFOCnt7 & 0x8000)
        {
            if (IPCFIFO7->IsFull())
                IPCFIFOCnt7 |= 0x4000;
            else
            {
                bool wasempty = IPCFIFO7->IsEmpty();
                IPCFIFO7->Write(val);
                if ((IPCFIFOCnt9 & 0x0400) && wasempty)
                    TriggerIRQ(0, IRQ_IPCRecv);
            }
        }
        return;

    case 0x040001A0:
        ROMSPIControl = val & 0xFFFF;
        // TODO: SPI shit
        return;
    case 0x040001A4:
        val &= ~0x00800000;
        ROMControl = val;
        if (val & 0x80000000) ROMStartTransfer(1);
        return;

    case 0x04000208: IME[1] = val & 0x1; return;
    case 0x04000210: IE[1] = val; return;
    case 0x04000214: IF[1] &= ~val; return;
    }

    if (addr >= 0x04000400 && addr < 0x04000520)
    {
        // sound I/O
        return;
    }

    printf("unknown ARM7 IO write32 %08X %08X\n", addr, val);
}

}
