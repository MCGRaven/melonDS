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

#ifndef GPU2D_H
#define GPU2D_H

class GPU2D
{
public:
    GPU2D(u32 num);
    ~GPU2D();

    void Reset();

    void SetFramebuffer(u16* buf);

    u8 Read8(u32 addr);
    u16 Read16(u32 addr);
    u32 Read32(u32 addr);
    void Write8(u32 addr, u8 val);
    void Write16(u32 addr, u16 val);
    void Write32(u32 addr, u32 val);

    void DrawScanline(u32 line);

private:
    u32 Num;
    u16* Framebuffer;

    u32 DispCnt;
    u16 BGCnt[4];

    u16 BGXPos[4];
    u16 BGYPos[4];

    void DrawScanline_Mode1(u32 line, u16* dst);
    void DrawBG_Text_4bpp(u32 line, u16* dst, u32 num);
};

#endif
