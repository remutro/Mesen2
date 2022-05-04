#include "stdafx.h"
#include "PCE/PcePpu.h"
#include "PCE/PceMemoryManager.h"
#include "PCE/PceControlManager.h"
#include "PCE/PceConstants.h"
#include "PCE/PceConsole.h"
#include "Shared/EmuSettings.h"
#include "Shared/RewindManager.h"
#include "Shared/Video/VideoDecoder.h"
#include "Shared/NotificationManager.h"
#include "EventType.h"

PcePpu::PcePpu(Emulator* emu, PceConsole* console)
{
	_emu = emu;
	_console = console;
	_vram = new uint16_t[0x8000];
	_spriteRam = new uint16_t[0x100];
	_paletteRam = new uint16_t[0x200];
	
	_outBuffer[0] = new uint16_t[PceConstants::MaxScreenWidth * PceConstants::ScreenHeight];
	_outBuffer[1] = new uint16_t[PceConstants::MaxScreenWidth * PceConstants::ScreenHeight];
	_currentOutBuffer = _outBuffer[0];

	memset(_outBuffer[0], 0, PceConstants::MaxScreenWidth * PceConstants::ScreenHeight * sizeof(uint16_t));
	memset(_outBuffer[1], 0, PceConstants::MaxScreenWidth * PceConstants::ScreenHeight * sizeof(uint16_t));

	_emu->GetSettings()->InitializeRam(_vram, 0x10000);
	_emu->GetSettings()->InitializeRam(_spriteRam, 0x200);
	_emu->GetSettings()->InitializeRam(_paletteRam, 0x400);
	for(int i = 0; i < 0x200; i++) {
		_paletteRam[i] &= 0x1FF;
	}

	//These values can't ever be 0, init them to a possible value
	_state.ColumnCount = 32;
	_state.RowCount = 32;
	_state.VramAddrIncrement = 1;
	_state.VceClockDivider = 4;

	_state.HvReg.HorizDisplayWidth = 0x1F;
	_state.HvLatch.HorizDisplayWidth = 0x1F;
	_state.HvReg.VertDisplayWidth = 239;
	_state.HvLatch.VertDisplayWidth = 239;
	_state.VceScanlineCount = 262;

	_emu->RegisterMemory(MemoryType::PceVideoRam, _vram, 0x8000 * sizeof(uint16_t));
	_emu->RegisterMemory(MemoryType::PcePaletteRam, _paletteRam, 0x200 * sizeof(uint16_t));
	_emu->RegisterMemory(MemoryType::PceSpriteRam, _spriteRam, 0x100 * sizeof(uint16_t));
}

PcePpu::~PcePpu()
{
	delete[] _vram;
	delete[] _paletteRam;
	delete[] _spriteRam;
	delete[] _outBuffer[0];
	delete[] _outBuffer[1];
}

PcePpuState& PcePpu::GetState()
{
	return _state;
}

uint16_t* PcePpu::GetScreenBuffer()
{
	return _currentOutBuffer;
}

uint16_t* PcePpu::GetPreviousScreenBuffer()
{
	return _currentOutBuffer == _outBuffer[0] ? _outBuffer[1] : _outBuffer[0];
}

uint16_t PcePpu::DotsToClocks(int dots)
{
	return dots * _state.VceClockDivider;
}

void PcePpu::Exec()
{
	if(_state.SatbTransferRunning) {
		ProcessSatbTransfer();
	}

	if(_rowHasSprite0) {
		DrawScanline();
	}

	if(_hModeCounter <= 3 || _nextEventCounter <= 3) {
		ProcessVdcEvents();
	} else {
		_hModeCounter -= 3;
		if(_nextEventCounter > 0) {
			_nextEventCounter -= 3;
		}
		_state.HClock += 3;
	}

	if(_state.HClock == 1365) {
		ProcessEndOfScanline();
	}

	_emu->ProcessPpuCycle<CpuType::Pce>();
}

void PcePpu::TriggerHdsIrqs()
{
	if(_needVertBlankIrq) {
		ProcessEndOfVisibleFrame();
	}
	if(_hasSpriteOverflow && _state.EnableOverflowIrq) {
		_state.SpriteOverflow = true;
		_console->GetMemoryManager()->SetIrqSource(PceIrqSource::Irq1);
	}
	_hasSpriteOverflow = false;
}

void PcePpu::ProcessVdcEvents()
{
	for(int i = 0; i < 3; i++) {
		_state.HClock++;

		if(--_hModeCounter == 0) {
			DrawScanline();
			SetHorizontalMode((PcePpuModeH)(((int)_hMode + 1) % 4));
		}

		if(_nextEventCounter && --_nextEventCounter == 0) {
			ProcessEvent();
		}
	}
}

void PcePpu::ProcessEvent()
{
	DrawScanline();

	switch(_nextEvent) {
		case PceVdcEvent::LatchScrollY:
			IncScrollY();
			_nextEvent = PceVdcEvent::LatchScrollX;
			_nextEventCounter = DotsToClocks(1);
			break;

		case PceVdcEvent::LatchScrollX:
			_state.HvLatch.BgScrollX = _state.HvReg.BgScrollX;
			_nextEvent = PceVdcEvent::HdsIrqTrigger;
			_nextEventCounter = DotsToClocks(7);
			if(!_state.BurstModeEnabled) {
				_state.BackgroundEnabled = _state.NextBackgroundEnabled;
				_state.SpritesEnabled = _state.NextSpritesEnabled;
			}
			break;

		case PceVdcEvent::HdsIrqTrigger:
			TriggerHdsIrqs();
			_nextEvent = PceVdcEvent::None;
			_nextEventCounter = UINT16_MAX;
			break;

		case PceVdcEvent::IncRcrCounter:
			IncrementRcrCounter();
			_nextEvent = PceVdcEvent::None;
			_nextEventCounter = UINT16_MAX;
			break;
	}
}

void PcePpu::SetHorizontalMode(PcePpuModeH hMode)
{
	_hMode = hMode;
	switch(_hMode) {
		case PcePpuModeH::Hds:
			_hModeCounter = DotsToClocks((_state.HvLatch.HorizDisplayStart + 1) * 8);
			//LogDebug("H: " + std::to_string(_state.HClock) + " - HDS");
			break;

		case PcePpuModeH::Hdw:
			_needRcrIncrement = true;
			_nextEvent = PceVdcEvent::IncRcrCounter;
			_nextEventCounter = DotsToClocks((_state.HvLatch.HorizDisplayWidth - 1) * 8) + 2;
			_hModeCounter = DotsToClocks((_state.HvLatch.HorizDisplayWidth + 1) * 8);
			//LogDebug("H: " + std::to_string(_state.HClock) + " - HDW start");
			break;

		case PcePpuModeH::Hde:
			_loadBgStart = UINT16_MAX;
			_evalStartCycle = UINT16_MAX;
			_loadSpriteStart = _state.HClock;

			_hModeCounter = DotsToClocks((_state.HvLatch.HorizDisplayEnd + 1) * 8);
			//LogDebug("H: " + std::to_string(_state.HClock) + " - HDE");
			break;

		case PcePpuModeH::Hsw:
			_loadBgStart = UINT16_MAX;
			_evalStartCycle = UINT16_MAX;
			_hModeCounter = DotsToClocks((_state.HvLatch.HorizSyncWidth + 1) * 8);
			ProcessHorizontalSyncStart();
			//LogDebug("H: " + std::to_string(_state.HClock) + " - HSW");
			break;
	}
}

void PcePpu::ProcessVerticalSyncStart()
{
	//Latch VSW/VDS/VDW/VCR at the start of each vertical sync?
	_state.HvLatch.VertSyncWidth = _state.HvReg.VertSyncWidth;
	_state.HvLatch.VertDisplayStart = _state.HvReg.VertDisplayStart;
	_state.HvLatch.VertDisplayWidth = _state.HvReg.VertDisplayWidth;
	_state.HvLatch.VertEndPosVcr = _state.HvReg.VertEndPosVcr;
}

void PcePpu::ProcessHorizontalSyncStart()
{
	//Latch HSW/HDS/HDW/HDE at the start of each horizontal sync?
	_state.HvLatch.HorizSyncWidth = _state.HvReg.HorizSyncWidth;
	_state.HvLatch.HorizDisplayStart = _state.HvReg.HorizDisplayStart;
	_state.HvLatch.HorizDisplayWidth = _state.HvReg.HorizDisplayWidth;
	_state.HvLatch.HorizDisplayEnd = _state.HvReg.HorizDisplayEnd;

	_nextEvent = PceVdcEvent::None;
	_nextEventCounter = UINT16_MAX;
	_tileCount = 0;
	_screenOffsetX = 0;

	uint16_t displayStart = _state.HClock + _hModeCounter + DotsToClocks((_state.HvLatch.HorizDisplayStart + 1) * 8);

	if(displayStart - DotsToClocks(24) >= PceConstants::ClockPerScanline) {
		return;
	}

	//Calculate when sprite evaluation, sprite fetching and bg fetching will occur on the scanline
	if(_vMode == PcePpuModeV::Vdw || _state.RcrCounter == _state.VceScanlineCount - 1) {
		uint16_t displayWidth = DotsToClocks((_state.HvLatch.HorizDisplayWidth + 1) * 8);

		//Sprite evaluation runs on all visible scanlines + the scanline before the picture starts
		uint16_t spriteEvalStart = displayStart - DotsToClocks(8);
		_evalStartCycle = spriteEvalStart;
		_evalLastCycle = 0;
		_evalEndCycle = std::min<uint16_t>(PceConstants::ClockPerScanline, spriteEvalStart + displayWidth + DotsToClocks(8));

		if(_vMode == PcePpuModeV::Vdw) {
			//Turn on BG tile fetching
			uint16_t bgFetchStart = displayStart - DotsToClocks(16);
			_loadBgStart = bgFetchStart;
			_loadBgLastCycle = 0;
			_loadBgEnd = std::min<uint16_t>(PceConstants::ClockPerScanline, bgFetchStart + displayWidth + DotsToClocks(16));
		}
	}

	uint16_t eventClocks;
	if(_vMode == PcePpuModeV::Vdw) {
		_nextEvent = PceVdcEvent::LatchScrollY;

		//Less than 33 causes Asuka 120% to have a flickering line
		//Either the RCR interrupt is too early, or the latching was too late
		eventClocks = DotsToClocks(33);
	} else {
		_nextEvent = PceVdcEvent::HdsIrqTrigger;
		eventClocks = DotsToClocks(24);
	}

	if((int16_t)displayStart - (int16_t)eventClocks <= (int16_t)_state.HClock) {
		ProcessEvent();
	} else {
		_nextEventCounter = displayStart - eventClocks - _state.HClock;
	}
}

void PcePpu::ProcessSpriteEvaluation()
{
	if(_state.HClock < _evalStartCycle || _hasSpriteOverflow || _evalLastCycle >= 64 || _state.BurstModeEnabled) {
		return;
	}

	uint16_t end = (std::min(_evalEndCycle, _state.HClock) - _evalStartCycle) / _state.VceClockDivider / 4;
	if(_evalLastCycle >= end) {
		return;
	}

	//LogDebug("SPR EVAL: " + std::to_string(_evalLastCycle) + " -> " + std::to_string(end - 1));

	if(_evalLastCycle == 0) {
		LoadSpriteTiles();
		_spriteCount = 0;
		_spriteRow = (_state.RcrCounter + 1) % _state.VceScanlineCount;
	}

	for(uint16_t i = _evalLastCycle; i < end; i++) {
		//4 VDC clocks is taken for each sprite
		if(i >= 64) {
			break;
		}

		int16_t y = (int16_t)(_spriteRam[i * 4] & 0x3FF) - 64;
		if(_spriteRow < y) {
			//Sprite not visible on this line
			continue;
		}

		uint8_t height;
		uint16_t flags = _spriteRam[i * 4 + 3];
		switch((flags >> 12) & 0x03) {
			default:
			case 0: height = 16; break;
			case 1: height = 32; break;
			case 2: case 3: height = 64; break;
		}

		if(_spriteRow >= y + height) {
			//Sprite not visible on this line
			continue;
		}

		uint16_t spriteRow = _spriteRow - y;

		bool verticalMirror = (flags & 0x8000) != 0;
		bool horizontalMirror = (flags & 0x800) != 0;

		int yOffset = 0;
		int rowOffset = 0;
		if(verticalMirror) {
			yOffset = (height - spriteRow - 1) & 0x0F;
			rowOffset = (height - spriteRow - 1) >> 4;
		} else {
			yOffset = spriteRow & 0x0F;
			rowOffset = spriteRow >> 4;
		}

		uint16_t tileIndex = (_spriteRam[i * 4 + 2] & 0x7FF) >> 1;
		bool loadSp23 = (_spriteRam[i * 4 + 2] & 0x01) != 0;
		uint8_t width = (flags & 0x100) ? 32 : 16;
		if(width == 32) {
			tileIndex &= ~0x01;
		}
		if(height == 32) {
			tileIndex &= ~0x02;
		} else if(height == 64) {
			tileIndex &= ~0x06;
		}
		uint16_t spriteTileY = tileIndex | (rowOffset << 1);

		for(int x = 0; x < width; x+=16) {
			if(_spriteCount >= 16) {
				_hasSpriteOverflow = true;
				break;
			} else {
				int columnOffset;
				if(horizontalMirror) {
					columnOffset = (width - x - 1) >> 4;
				} else {
					columnOffset = x >> 4;
				}

				uint16_t spriteTile = spriteTileY | columnOffset;
				_sprites[_spriteCount].Index = i;
				_sprites[_spriteCount].X = (int16_t)(_spriteRam[i * 4 + 1] & 0x3FF) - 32 + x;
				_sprites[_spriteCount].TileAddress = spriteTile * 64 + yOffset;
				_sprites[_spriteCount].HorizontalMirroring = horizontalMirror;
				_sprites[_spriteCount].ForegroundPriority = (flags & 0x80) != 0;
				_sprites[_spriteCount].Palette = (flags & 0x0F);
				_sprites[_spriteCount].LoadSp23 = loadSp23;
				_spriteCount++;
			}
		}
	}

	_evalLastCycle = end;
}

void PcePpu::LoadBackgroundTiles()
{
	if(_state.HClock < _loadBgStart || _state.BurstModeEnabled) {
		return;
	}

	uint16_t end = (std::min(_loadBgEnd, _state.HClock) - _loadBgStart) / _state.VceClockDivider;

	if(_loadBgLastCycle >= end) {
		return;
	}

	//LogDebug("BG: " + std::to_string(_loadBgLastCycle) + " -> " + std::to_string(end - 1));

	uint16_t columnMask = _state.ColumnCount - 1;
	uint16_t scrollOffset = _state.HvLatch.BgScrollX >> 3;
	uint16_t row = (_state.HvLatch.BgScrollY) & ((_state.RowCount * 8) - 1);

	if(_state.VramAccessMode == 0) {
		for(uint16_t i = _loadBgLastCycle; i < end; i++) {
			switch(i & 0x07) {
				//CPU can access VRAM
				case 0: case 2: case 4: case 6: _allowVramAccess = true; break;

				case 1: LoadBatEntry(scrollOffset, columnMask, row); break;
				case 3: _allowVramAccess = false; break; //Unused BAT read?
				case 5: LoadTileDataCg0(row); break;
				case 7: LoadTileDataCg1(row); break;
			}
		}
	} else if(_state.VramAccessMode == 3) {
		//Mode 3 is 4 cycles per read, CPU has no VRAM access, only 2BPP
		LoadBackgroundTilesWidth4(end, scrollOffset, columnMask, row);
	} else {
		//Mode 1/2 are 2 cycles per read
		LoadBackgroundTilesWidth2(end, scrollOffset, columnMask, row);
	}

	_loadBgLastCycle = end;
}

void PcePpu::LoadBackgroundTilesWidth2(uint16_t end, uint16_t scrollOffset, uint16_t columnMask, uint16_t row)
{
	for(uint16_t i = _loadBgLastCycle; i < end; i++) {
		switch(i & 0x07) {
			case 1: LoadBatEntry(scrollOffset, columnMask, row); break;
			case 2: _allowVramAccess = true; break; //CPU
			case 5: LoadTileDataCg0(row); break;
			case 7: LoadTileDataCg1(row); break;
		}
	}
}

void PcePpu::LoadBackgroundTilesWidth4(uint16_t end, uint16_t scrollOffset, uint16_t columnMask, uint16_t row)
{
	_allowVramAccess = false;
	for(uint16_t i = _loadBgLastCycle; i < end; i++) {
		switch(i & 0x07) {
			case 3: LoadBatEntry(scrollOffset, columnMask, row); break;

			case 7:
				//Load CG0 or CG1 based on CG mode flag
				_tiles[_tileCount].TileData[0] = ReadVram(_tiles[_tileCount].TileAddr + (row & 0x07) + (_state.CgMode ? 8 : 0));
				_tiles[_tileCount].TileData[1] = 0;
				_allowVramAccess = false;
				_tileCount++;
				break;
		}
	}
}

void PcePpu::LoadBatEntry(uint16_t scrollOffset, uint16_t columnMask, uint16_t row)
{
	uint16_t tileColumn = (scrollOffset + _tileCount) & columnMask;
	uint16_t batEntry = ReadVram((row >> 3) * _state.ColumnCount + tileColumn);
	_tiles[_tileCount].Palette = batEntry >> 12;
	_tiles[_tileCount].TileAddr = ((batEntry & 0xFFF) * 16);
	_allowVramAccess = false;
}

void PcePpu::LoadTileDataCg0(uint16_t row)
{
	_tiles[_tileCount].TileData[0] = ReadVram(_tiles[_tileCount].TileAddr + (row & 0x07));
	_allowVramAccess = false;
}

void PcePpu::LoadTileDataCg1(uint16_t row)
{
	_tiles[_tileCount].TileData[1] = ReadVram(_tiles[_tileCount].TileAddr + (row & 0x07) + 8);
	_allowVramAccess = false;
	_tileCount++;
}

uint16_t PcePpu::ReadVram(uint16_t addr)
{
	//TODO validate - Camp California seems to expect empty sprites if tile index is out of bounds (>= $200)
	return addr >= 0x8000 ? 0 : _vram[addr];
}

void PcePpu::LoadSpriteTiles()
{
	_drawSpriteCount = 0;
	_rowHasSprite0 = false;

	if(_state.BurstModeEnabled) {
		return;
	}

	uint16_t clockCount = _loadSpriteStart > _loadBgStart ? (PceConstants::ClockPerScanline - _loadSpriteStart) + _loadBgStart : (_loadBgStart - _loadSpriteStart);
	bool hasSprite0 = false;
	if(_state.SpriteAccessMode != 1) {
		//Modes 0/2/3 load 4 words over 4, 8 or 16 VDC clocks
		uint16_t clocksPerSprite;
		switch(_state.SpriteAccessMode) {
			default: case 0: clocksPerSprite = 4; break;
			case 2: clocksPerSprite = 8; break;
			case 3: clocksPerSprite = 16; break;
		}

		_drawSpriteCount = std::min<uint16_t>(_spriteCount, clockCount / _state.VceClockDivider / clocksPerSprite);
		for(int i = 0; i < _drawSpriteCount; i++) {
			PceSpriteInfo& spr = _drawSprites[i];
			spr = _sprites[i];
			uint16_t addr = spr.TileAddress;
			spr.TileData[0] = ReadVram(addr);
			spr.TileData[1] = ReadVram(addr + 16);
			spr.TileData[2] = ReadVram(addr + 32);
			spr.TileData[3] = ReadVram(addr + 48);
			hasSprite0 |= spr.Index == 0;
		}
	} else {
		//Mode 1 uses 2BPP sprites, 4 clocks per sprite
		_drawSpriteCount = std::min<uint16_t>(_spriteCount, clockCount / _state.VceClockDivider / 4);
		for(int i = 0; i < _drawSpriteCount; i++) {
			PceSpriteInfo& spr = _drawSprites[i];
			spr = _sprites[i];
			
			//Load SP0/SP1 or SP2/SP3 based on flag
			uint16_t addr = spr.TileAddress + (spr.LoadSp23 ? 32 : 0);
			spr.TileData[0] = ReadVram(addr);
			spr.TileData[1] = ReadVram(addr + 16);
			spr.TileData[2] = 0;
			spr.TileData[3] = 0;
			hasSprite0 |= spr.Index == 0;
		}
	}

	if(hasSprite0 && _drawSpriteCount > 1) {
		//Force VDC emulation to run on each CPU cycle, to ensure any sprite 0 hit IRQ is triggerd at the correct time
		_rowHasSprite0 = true;
	}
}

void PcePpu::ProcessSatbTransfer()
{
	//This takes 1024 VDC cycles (so 2048/3072/4096 master clocks depending on VCE/VDC speed)
	//1 word transfered every 4 dots (8 to 16 master clocks, depending on VCE clock divider)
	_state.SatbTransferNextWordCounter += 3;
	if(_state.SatbTransferNextWordCounter / _state.VceClockDivider >= 4) {
		_state.SatbTransferNextWordCounter -= 4 * _state.VceClockDivider;

		int i = _state.SatbTransferOffset;
		uint16_t value = ReadVram(_state.SatbBlockSrc + i);
		_emu->ProcessPpuWrite<CpuType::Pce>(i << 1, value & 0xFF, MemoryType::PceSpriteRam);
		_emu->ProcessPpuWrite<CpuType::Pce>((i << 1) + 1, value >> 8, MemoryType::PceSpriteRam);
		_spriteRam[i] = value;

		_state.SatbTransferOffset++;

		if(_state.SatbTransferOffset == 0) {
			_state.SatbTransferRunning = false;

			if(_state.VramSatbIrqEnabled) {
				_state.SatbTransferDone = true;
				_console->GetMemoryManager()->SetIrqSource(PceIrqSource::Irq1);
			}
		}
	}
}

void PcePpu::IncrementRcrCounter()
{
	_state.RcrCounter++;

	if(_needBgScrollYInc) {
		IncScrollY();
	}
	_needBgScrollYInc = true;
	_needRcrIncrement = false;

	_vModeCounter--;
	if(_vModeCounter == 0) {
		_vMode = (PcePpuModeV)(((int)_vMode + 1) % 4);
		switch(_vMode) {
			default:
			case PcePpuModeV::Vds:
				_vModeCounter = _state.HvLatch.VertDisplayStart + 2;
				break;

			case PcePpuModeV::Vdw:
				_vModeCounter = _state.HvLatch.VertDisplayWidth + 1;
				_state.RcrCounter = 0;
				break;

			case PcePpuModeV::Vde:
				_vModeCounter = _state.HvLatch.VertEndPosVcr;
				break;

			case PcePpuModeV::Vsw:
				ProcessVerticalSyncStart();
				_vModeCounter = _state.HvLatch.VertSyncWidth + 1;
				break;
		}
	}

	if(_vMode == PcePpuModeV::Vde && _state.RcrCounter == _state.HvLatch.VertDisplayWidth + 1) {
		_needVertBlankIrq = true;
		_verticalBlankDone = true;
	}

	//This triggers ~12 VDC cycles before the end of the visible part of the scanline
	if(_state.EnableScanlineIrq && _state.RcrCounter == (int)_state.RasterCompareRegister - 0x40) {
		_state.ScanlineDetected = true;
		_console->GetMemoryManager()->SetIrqSource(PceIrqSource::Irq1);
	}
}

void PcePpu::IncScrollY()
{
	if(_state.RcrCounter == 0) {
		_state.HvLatch.BgScrollY = _state.HvReg.BgScrollY;
	} else {
		if(_state.BgScrollYUpdatePending) {
			_state.HvLatch.BgScrollY = _state.HvReg.BgScrollY;
			_state.BgScrollYUpdatePending = false;
		}

		_state.HvLatch.BgScrollY++;
	}
	_needBgScrollYInc = false;
}

void PcePpu::ProcessEndOfScanline()
{
	DrawScanline();

	_state.HClock = 0;
	_state.Scanline++;

	if(_state.Scanline >= 14 && _state.Scanline < 256) {
		uint16_t row = _state.Scanline - 14;
		if(row == 0) {
			_currentOutBuffer = _currentOutBuffer == _outBuffer[0] ? _outBuffer[1] : _outBuffer[0];
			_currentClockDividers = _currentOutBuffer == _outBuffer[0] ? _rowVceClockDivider[0] : _rowVceClockDivider[1];
		}
		_currentClockDividers[row] = _state.VceClockDivider;
	}

	if(_state.Scanline == 256) {
		_state.FrameCount++;
		SendFrame();
	} else if(_state.Scanline >= _state.VceScanlineCount) {
		//Update flags that were locked during burst mode
		_state.Scanline = 0;
		_verticalBlankDone = false;
		_state.BurstModeEnabled = !_state.NextBackgroundEnabled && !_state.NextSpritesEnabled;
		_state.BackgroundEnabled = _state.NextBackgroundEnabled;
		_state.SpritesEnabled = _state.NextSpritesEnabled;

		_emu->ProcessEvent(EventType::StartFrame);
	}

	if(_needRcrIncrement) {
		IncrementRcrCounter();
	}

	if(_hMode == PcePpuModeH::Hdw) {
		//Display output was interrupted by hblank, start loading sprites in ~20 dots. (approximate, based on timing test)
		_loadSpriteStart = DotsToClocks(20);
	}
	
	//VCE sets HBLANK to low every 1365 clocks, interrupting what 
	//the VDC was doing and starting a 16-pixel HSW phase
	_hMode = PcePpuModeH::Hsw;
	_loadBgStart = UINT16_MAX;
	_evalStartCycle = UINT16_MAX;
	_hModeCounter = DotsToClocks(16);
	ProcessHorizontalSyncStart();

	_xStart = 0;
	_lastDrawHClock = 0;

	if(_state.Scanline == _state.VceScanlineCount - 3) {
		//VCE sets VBLANK for 3 scanlines at the end of every frame
		_vMode = PcePpuModeV::Vsw;
		ProcessVerticalSyncStart();
		_vModeCounter = _state.HvLatch.VertSyncWidth + 1;
	} else if(_state.Scanline == _state.VceScanlineCount - 2) {
		if(!_verticalBlankDone) {
			_needVertBlankIrq = true;
		}
	}
}

void PcePpu::ProcessEndOfVisibleFrame()
{
	//End of display, trigger irq?
	if(_state.SatbTransferPending || _state.RepeatSatbTransfer) {
		_state.SatbTransferPending = false;
		_state.SatbTransferRunning = true;
		_state.SatbTransferNextWordCounter = 0;
		_state.SatbTransferOffset = 0;
	}

	if(_state.EnableVerticalBlankIrq) {
		_state.VerticalBlank = true;
		_console->GetMemoryManager()->SetIrqSource(PceIrqSource::Irq1);
	}

	_needVertBlankIrq = false;
}

uint8_t PcePpu::GetTilePixelColor(const uint16_t chrData[2], const uint8_t shift)
{
	return (
		((chrData[0] >> shift) & 0x01) |
		((chrData[0] >> (7 + shift)) & 0x02) |
		(((chrData[1] >> shift) & 0x01) << 2) |
		(((chrData[1] >> (7 + shift)) & 0x02) << 2)
	);
}

uint8_t PcePpu::GetSpritePixelColor(const uint16_t chrData[4], const uint8_t shift)
{
	return (
		((chrData[0] >> shift) & 0x01) |
		(((chrData[1] >> shift) & 0x01) << 1) |
		(((chrData[2] >> shift) & 0x01) << 2) |
		(((chrData[3] >> shift) & 0x01) << 3)
	);
}

void PcePpu::DrawScanline()
{
	if(_state.Scanline < 14 || _state.Scanline >= 256) {
		//Only 242 rows can be shown
		return;
	}

	ProcessSpriteEvaluation();
	LoadBackgroundTiles();

	uint16_t* out = _rowBuffer;

	uint16_t pixelsToDraw = (_state.HClock - _lastDrawHClock) / _state.VceClockDivider;
	uint16_t xStart = _xStart;
	uint16_t xMax = _xStart + pixelsToDraw;

	bool inPicture = _hMode == PcePpuModeH::Hdw && _tileCount > 0;

	if(inPicture && (_state.BackgroundEnabled || _state.SpritesEnabled)) {
		for(; xStart < xMax; xStart++) {
			uint8_t bgColor = 0;
			uint16_t outColor = _paletteRam[0];
			if(_state.BackgroundEnabled) {
				uint16_t screenX = (_state.HvLatch.BgScrollX & 0x07) + _screenOffsetX;
				uint16_t column = screenX >> 3;
				bgColor = GetTilePixelColor(_tiles[column].TileData, 7 - (screenX & 0x07));
				if(bgColor != 0) {
					outColor = _paletteRam[_tiles[column].Palette * 16 + bgColor];
				}
			}

			if(_state.SpritesEnabled) {
				uint8_t sprColor;
				bool checkSprite0Hit = false;
				for(uint16_t i = 0; i < _drawSpriteCount; i++) {
					int16_t xOffset = _screenOffsetX - _drawSprites[i].X;
					if(xOffset >= 0 && xOffset < 16) {
						if(!_drawSprites[i].HorizontalMirroring) {
							xOffset = 15 - xOffset;
						}

						sprColor = GetSpritePixelColor(_drawSprites[i].TileData, xOffset);

						if(sprColor != 0) {
							if(checkSprite0Hit) {
								if(_state.EnableCollisionIrq) {
									_state.Sprite0Hit = true;
									_console->GetMemoryManager()->SetIrqSource(PceIrqSource::Irq1);
								}
							} else {
								if(bgColor == 0 || _drawSprites[i].ForegroundPriority) {
									outColor = _paletteRam[256 + _drawSprites[i].Palette * 16 + sprColor];
								}
							}

							if(_drawSprites[i].Index == 0) {
								checkSprite0Hit = true;
							} else {
								break;
							}
						}
					}
				}
			}

			out[xStart] = outColor;
			_screenOffsetX++;
		}
	} else if(inPicture) {
		uint16_t color = _paletteRam[0];
		for(; xStart < xMax; xStart++) {
			//In picture, but BG is not enabled, draw bg color
			out[xStart] = color;
		}
	} else {
		uint16_t color = _paletteRam[16 * 16];
		for(; xStart < xMax; xStart++) {
			//Output hasn't started yet, display overscan color
			out[xStart] = color;
		}
	}

	if(_state.HClock == 1365) {
		uint32_t offset = (_state.Scanline - 14) * PceConstants::MaxScreenWidth;
		memcpy(_currentOutBuffer + offset, _rowBuffer, PceConstants::ClockPerScanline / _state.VceClockDivider * sizeof(uint16_t));
	}

	_xStart = xStart;
	_lastDrawHClock = _state.HClock / _state.VceClockDivider * _state.VceClockDivider;
}

void PcePpu::SendFrame()
{
	_emu->ProcessEvent(EventType::EndFrame);
	_emu->GetNotificationManager()->SendNotification(ConsoleNotificationType::PpuFrameDone, _currentOutBuffer);

	bool forRewind = _emu->GetRewindManager()->IsRewinding();

	RenderedFrame frame(_currentOutBuffer, 512, PceConstants::ScreenHeight * 2, 0.5, _state.FrameCount, _console->GetControlManager()->GetPortStates());
	frame.Data = _currentClockDividers;
	_emu->GetVideoDecoder()->UpdateFrame(frame, forRewind, forRewind);

	_emu->ProcessEndOfFrame();

	_console->GetControlManager()->UpdateInputState();
	_console->GetControlManager()->UpdateControlDevices();
}

void PcePpu::LoadReadBuffer()
{
	_state.ReadBuffer = ReadVram(_state.MemAddrRead);
	_emu->ProcessPpuRead<CpuType::Pce>((_state.MemAddrRead << 1), (uint8_t)_state.ReadBuffer, MemoryType::PceVideoRam);
	_emu->ProcessPpuRead<CpuType::Pce>((_state.MemAddrRead << 1) + 1, (uint8_t)(_state.ReadBuffer >> 8), MemoryType::PceVideoRam);
	_pendingMemoryRead = false;
}

uint8_t PcePpu::ReadVdc(uint16_t addr)
{
	DrawScanline();

	switch(addr & 0x03) {
		default:
		case 0: {
			//TODO BSY
			uint8_t result = 0;
			result |= _state.VerticalBlank ? 0x20 : 0x00;
			result |= _state.VramTransferDone ? 0x10 : 0x00;
			result |= _state.SatbTransferDone ? 0x08 : 0x00;
			result |= _state.ScanlineDetected ? 0x04 : 0x00;
			result |= _state.SpriteOverflow ? 0x02 : 0x00;
			result |= _state.Sprite0Hit ? 0x01 : 0x00;

			_state.VerticalBlank = false;
			_state.VramTransferDone = false;
			_state.SatbTransferDone = false;
			_state.ScanlineDetected = false;
			_state.SpriteOverflow = false;
			_state.Sprite0Hit = false;

			_console->GetMemoryManager()->ClearIrqSource(PceIrqSource::Irq1);
			return result;
		}

		case 1: return 0; //Unused, reads return 0

		//Reads to 2/3 will always return the read buffer, but the
		//read address will only increment when register 2 is selected
		case 2: 
			if(_pendingMemoryRead) {
				WaitForVramAccess();
				LoadReadBuffer();
			}
			return (uint8_t)_state.ReadBuffer;

		case 3:
			if(_pendingMemoryRead) {
				WaitForVramAccess();
				LoadReadBuffer();
			}

			uint8_t value = _state.ReadBuffer >> 8;
			if(_state.CurrentReg == 0x02) {
				_state.MemAddrRead += _state.VramAddrIncrement;
				_pendingMemoryRead = true;
			}
			return value;
	}
}

bool PcePpu::IsVramAccessBlocked()
{
	if(_state.BurstModeEnabled) {
		return false;
	}

	bool inBgFetch = _state.HClock >= _loadBgStart && _state.HClock < _loadBgEnd;
	//TODO timing:
	//does disabling sprites allow vram access during hblank?
	//can you access vram after the VDC is done loading sprites for that scanline?
	return (
		_state.SatbTransferRunning || 
		(inBgFetch && !_allowVramAccess) || 
		(_state.SpritesEnabled && !inBgFetch && _vMode == PcePpuModeV::Vdw)
	);
}

void PcePpu::WaitForVramAccess()
{
	while(IsVramAccessBlocked()) {
		_console->GetMemoryManager()->ExecFast();
		DrawScanline();
	}
}

void PcePpu::WriteVdc(uint16_t addr, uint8_t value)
{
	DrawScanline();

	switch(addr & 0x03) {
		case 0: _state.CurrentReg = value & 0x1F; break;

		case 1: break; //Unused, writes do nothing

		case 2:
		case 3:
			bool msb = (addr & 0x03) == 0x03;

			switch(_state.CurrentReg) {
				case 0x00: UpdateReg(_state.MemAddrWrite, value, msb); break;

				case 0x01:
					UpdateReg(_state.MemAddrRead, value, msb);
					if(msb) {
						_pendingMemoryRead = true;
					}
					break;

				case 0x02:
					UpdateReg(_state.VramData, value, msb);
					if(msb) {
						WaitForVramAccess();

						if(_state.MemAddrWrite < 0x8000) {
							//Ignore writes to mirror at $8000+
							//TODO timing
							_emu->ProcessPpuWrite<CpuType::Pce>(_state.MemAddrWrite << 1, _state.VramData & 0xFF, MemoryType::PceVideoRam);
							_emu->ProcessPpuWrite<CpuType::Pce>((_state.MemAddrWrite << 1) + 1, value, MemoryType::PceVideoRam);
							_vram[_state.MemAddrWrite] = _state.VramData;
						}

						_state.MemAddrWrite += _state.VramAddrIncrement;
					}
					break;

				case 0x05:
					if(msb) {
						//TODO output select
						//TODO dram refresh
						switch((value >> 3) & 0x03) {
							case 0: _state.VramAddrIncrement = 1; break;
							case 1: _state.VramAddrIncrement = 0x20; break;
							case 2: _state.VramAddrIncrement = 0x40; break;
							case 3: _state.VramAddrIncrement = 0x80; break;
						}
					} else {
						_state.EnableCollisionIrq = (value & 0x01) != 0;
						_state.EnableOverflowIrq = (value & 0x02) != 0;
						_state.EnableScanlineIrq = (value & 0x04) != 0;
						_state.EnableVerticalBlankIrq = (value & 0x08) != 0;
						
						_state.OutputVerticalSync = ((value & 0x30) >> 4) >= 2;
						_state.OutputHorizontalSync = ((value & 0x30) >> 4) >= 1;

						_state.NextSpritesEnabled = (value & 0x40) != 0;
						_state.NextBackgroundEnabled = (value & 0x80) != 0;
					}
					break;

				case 0x06: UpdateReg<0x3FF>(_state.RasterCompareRegister, value, msb); break;
				case 0x07: UpdateReg<0x3FF>(_state.HvReg.BgScrollX, value, msb); break;
				case 0x08:
					UpdateReg<0x1FF>(_state.HvReg.BgScrollY, value, msb);
					_state.BgScrollYUpdatePending = true;
					break;

				case 0x09:
					if(!msb) {
						switch((value >> 4) & 0x03) {
							case 0: _state.ColumnCount = 32; break;
							case 1: _state.ColumnCount = 64; break;
							case 2: case 3: _state.ColumnCount = 128; break;
						}

						_state.RowCount = (value & 0x40) ? 64 : 32;

						_state.VramAccessMode = value & 0x03;
						_state.SpriteAccessMode = (value >> 2) & 0x03;
						_state.CgMode = (value & 0x80) != 0;
					}
					break;

				case 0x0A:
					if(msb) {
						_state.HvReg.HorizDisplayStart = value & 0x7F;
					} else {
						_state.HvReg.HorizSyncWidth = value & 0x1F;
					}
					break;

				case 0x0B:
					if(msb) {
						_state.HvReg.HorizDisplayEnd = value & 0x7F;
					} else {
						_state.HvReg.HorizDisplayWidth = value & 0x7F;
					}
					break;

				case 0x0C: 
					if(msb) {
						_state.HvReg.VertDisplayStart = value;
					} else {
						_state.HvReg.VertSyncWidth = value & 0x1F;
					}
					break;

				case 0x0D:
					UpdateReg<0x1FF>(_state.HvReg.VertDisplayWidth, value, msb);
					break;

				case 0x0E: 
					if(!msb) {
						_state.HvReg.VertEndPosVcr = value;
					}
					break;

				case 0x0F:
					if(!msb) {
						_state.VramSatbIrqEnabled = (value & 0x01) != 0;
						_state.VramVramIrqEnabled = (value & 0x02) != 0;
						_state.DecrementSrc = (value & 0x04) != 0;
						_state.DecrementDst = (value & 0x08) != 0;
						_state.RepeatSatbTransfer = (value & 0x10) != 0;
					}
					break;

				case 0x10: UpdateReg(_state.BlockSrc, value, msb); break;
				case 0x11: UpdateReg(_state.BlockDst, value, msb); break;
				case 0x12:
					UpdateReg(_state.BlockLen, value, msb);

					if(msb) {
						//TODO DMA TIMING
						do {
							_state.BlockLen--;

							if(_state.BlockDst < 0x8000) {
								//Ignore writes over $8000
								_vram[_state.BlockDst] = ReadVram(_state.BlockSrc);
							}

							_state.BlockSrc += (_state.DecrementSrc ? -1 : 1);
							_state.BlockDst += (_state.DecrementDst ? -1 : 1);

						} while(_state.BlockLen != 0xFFFF);

						if(_state.VramVramIrqEnabled) {
							_state.VramTransferDone = true;
							_console->GetMemoryManager()->SetIrqSource(PceIrqSource::Irq1);
						}
					}
					break;

				case 0x13:
					UpdateReg(_state.SatbBlockSrc, value, msb);
					if(msb) {
						_state.SatbTransferPending = true;
					}
					break;
			}
	}
}

uint8_t PcePpu::ReadVce(uint16_t addr)
{
	DrawScanline();

	switch(addr & 0x07) {
		default:
		case 0: return 0xFF; //write-only, reads return $FF
		case 1: return 0xFF; //unused, reads return $FF
		case 2: return 0xFF; //write-only, reads return $FF
		case 3: return 0xFF; //write-only, reads return $FF

		case 4: return _paletteRam[_state.PalAddr] & 0xFF;
		
		case 5: {
			uint8_t val = (_paletteRam[_state.PalAddr] >> 8) & 0x01;
			_state.PalAddr = (_state.PalAddr + 1) & 0x1FF;

			//Bits 1 to 7 are set to 1 when reading MSB
			return 0xFE | val;
		}

		case 6: return 0xFF; //unused, reads return $FF
		case 7: return 0xFF; //unused, reads return $FF
	}
}

void PcePpu::WriteVce(uint16_t addr, uint8_t value)
{
	DrawScanline();

	switch(addr & 0x07) {
		case 0x00:
			_state.VceScanlineCount = (value & 0x04) ? 263 : 262;
			switch(value & 0x03) {
				case 0: _state.VceClockDivider = 4; break;
				case 1: _state.VceClockDivider = 3; break;
				case 2: case 3: _state.VceClockDivider = 2; break;
			}
			//LogDebug("[Debug] VCE Clock divider: " + HexUtilities::ToHex(_state.VceClockDivider) + "  SL: " + std::to_string(_state.Scanline));
			break;

		case 0x01: break; //Unused, writes do nothing

		case 0x02: _state.PalAddr = (_state.PalAddr & 0x100) | value; break;
		case 0x03: _state.PalAddr = (_state.PalAddr & 0xFF) | ((value & 0x01) << 8); break;

		case 0x04:
			_emu->ProcessPpuWrite<CpuType::Pce>((_state.PalAddr << 1), value, MemoryType::PceVideoRam);
			_paletteRam[_state.PalAddr] = (_paletteRam[_state.PalAddr] & 0x100) | value;
			break;

		case 0x05:
			_emu->ProcessPpuWrite<CpuType::Pce>((_state.PalAddr << 1) + 1, value, MemoryType::PceVideoRam);
			_paletteRam[_state.PalAddr] = (_paletteRam[_state.PalAddr] & 0xFF) | ((value & 0x01) << 8);
			_state.PalAddr = (_state.PalAddr + 1) & 0x1FF;
			break;

		case 0x06: break; //Unused, writes do nothing
		case 0x07: break; //Unused, writes do nothing
	}
}