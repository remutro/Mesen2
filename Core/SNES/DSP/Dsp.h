#pragma once
#include "stdafx.h"
#include "SNES/DSP/DspVoice.h"
#include "SNES/DSP/DspTypes.h"
#include "Utilities/ISerializable.h"

class Emulator;
class Spc;

class Dsp final : public ISerializable
{
private:
	//KOF ($5C) must be initialized to $00, some games (Chester Cheetah, King of Dragons) do not initialize its value
	//This causes missing sound effects in both games.
	static constexpr uint8_t _initialValues[0x80] = {
		0x45,0x8B,0x5A,0x9A,0xE4,0x82,0x1B,0x78,0x00,0x00,0xAA,0x96,0x89,0x0E,0xE0,0x80,
		0x2A,0x49,0x3D,0xBA,0x14,0xA0,0xAC,0xC5,0x00,0x00,0x51,0xBB,0x9C,0x4E,0x7B,0xFF,
		0xF4,0xFD,0x57,0x32,0x37,0xD9,0x42,0x22,0x00,0x00,0x5B,0x3C,0x9F,0x1B,0x87,0x9A,
		0x6F,0x27,0xAF,0x7B,0xE5,0x68,0x0A,0xD9,0x00,0x00,0x9A,0xC5,0x9C,0x4E,0x7B,0xFF,
		0xEA,0x21,0x78,0x4F,0xDD,0xED,0x24,0x14,0x00,0x00,0x77,0xB1,0xD1,0x36,0xC1,0x67,
		0x52,0x57,0x46,0x3D,0x59,0xF4,0x87,0xA4,0x00,0x00,0x7E,0x44,0x00,0x4E,0x7B,0xFF,
		0x75,0xF5,0x06,0x97,0x10,0xC3,0x24,0xBB,0x00,0x00,0x7B,0x7A,0xE0,0x60,0x12,0x0F,
		0xF7,0x74,0x1C,0xE5,0x39,0x3D,0x73,0xC1,0x00,0x00,0x7A,0xB3,0xFF,0x4E,0x7B,0xFF
	};

	DspState _state = {};
	DspVoice _voices[8] = {};
	Emulator* _emu = nullptr;
	Spc* _spc = nullptr;

	//Output buffer
	uint16_t _outSampleCount = 0;
	int16_t _dspOutput[0x2000] = {};

	void UpdateCounter();
	
	int32_t CalculateFir(int index, int ch);

	void EchoStep22();
	void EchoStep23();
	void EchoStep24();
	void EchoStep25();
	void EchoStep26();
	void EchoStep27();
	void EchoStep28();
	void EchoStep29();
	void EchoStep30();

public:
	Dsp(Emulator* emu, Spc* spc);

	void Reset();

	DspState& GetState() { return _state; }
	bool IsMuted() { return false; }

	uint16_t GetSampleCount() { return _outSampleCount; }
	int16_t* GetSamples() { return _dspOutput; }
	void ResetOutput() { _outSampleCount = 0; }

	bool CheckCounter(int32_t rate);

	uint8_t Read(uint8_t reg) { return _state.Regs[reg]; }
	void Write(uint8_t reg, uint8_t value);

	uint8_t ReadReg(DspGlobalRegs reg) { return _state.Regs[(int)reg]; }
	void WriteReg(DspGlobalRegs reg, uint8_t value) { _state.Regs[(int)reg] = value; }

	static int16_t Clamp16(int32_t val)
	{
		if(val < INT16_MIN) {
			return INT16_MIN;
		} else if(val > INT16_MAX) {
			return INT16_MAX;
		}
		return val;
	}
	void Exec();

	void Serialize(Serializer& s) override;
};
