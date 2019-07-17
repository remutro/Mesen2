#pragma once
#include "stdafx.h"
#include "CpuTypes.h"
#include "PpuTypes.h"
#include "DebugTypes.h"

class Console;
class Cpu;
class Ppu;
class Spc;
class BaseCartridge;
class MemoryManager;
class EmuSettings;

class TraceLogger;
class ExpressionEvaluator;
class MemoryDumper;
class MemoryAccessCounter;
class Disassembler;
class BreakpointManager;
class PpuTools;
class CodeDataLogger;
class EventManager;
class CallstackManager;
class LabelManager;
class ScriptManager;

enum class EventType;
enum class EvalResultType : int32_t;

class Debugger
{
private:
	shared_ptr<Console> _console;
	shared_ptr<Cpu> _cpu;
	shared_ptr<Ppu> _ppu;
	shared_ptr<Spc> _spc;
	shared_ptr<MemoryManager> _memoryManager;
	shared_ptr<BaseCartridge> _cart;
	
	shared_ptr<EmuSettings> _settings;

	shared_ptr<ScriptManager> _scriptManager;
	shared_ptr<TraceLogger> _traceLogger;
	shared_ptr<MemoryDumper> _memoryDumper;
	shared_ptr<MemoryAccessCounter> _memoryAccessCounter;
	shared_ptr<CodeDataLogger> _codeDataLogger;
	shared_ptr<Disassembler> _disassembler;
	shared_ptr<BreakpointManager> _breakpointManager;
	shared_ptr<PpuTools> _ppuTools;
	shared_ptr<EventManager> _eventManager;
	shared_ptr<LabelManager> _labelManager;
	shared_ptr<CallstackManager> _callstackManager;
	shared_ptr<CallstackManager> _spcCallstackManager;

	unique_ptr<ExpressionEvaluator> _watchExpEval[(int)CpuType::Spc + 1];

	atomic<bool> _executionStopped;
	atomic<uint32_t> _breakRequestCount;
	atomic<uint32_t> _suspendRequestCount;

	atomic<int32_t> _cpuStepCount;
	atomic<int32_t> _spcStepCount;
	atomic<int32_t> _ppuStepCount;
	atomic<int32_t> _cpuBreakAddress;
	atomic<int32_t> _spcBreakAddress;
	atomic<int32_t> _breakScanline;
	
	bool _enableBreakOnUninitRead = false;

	uint8_t _prevOpCode = 0xFF;
	uint32_t _prevProgramCounter = 0;

	uint8_t _spcPrevOpCode = 0xFF;
	uint32_t _spcPrevProgramCounter = 0;

	void Reset();
	void SleepUntilResume(BreakSource source, MemoryOperationInfo *operation = nullptr, int breakpointId = -1);
	void ProcessStepConditions(uint8_t opCode, uint32_t currentPc);
	void ProcessBreakConditions(MemoryOperationInfo &operation, AddressInfo &addressInfo, BreakSource source = BreakSource::Unspecified);

public:
	Debugger(shared_ptr<Console> console);
	~Debugger();
	void Release();

	void ProcessCpuRead(uint32_t addr, uint8_t value, MemoryOperationType type);
	void ProcessCpuWrite(uint32_t addr, uint8_t value, MemoryOperationType type);

	void ProcessWorkRamRead(uint32_t addr, uint8_t value);
	void ProcessWorkRamWrite(uint32_t addr, uint8_t value);

	void ProcessSpcRead(uint16_t addr, uint8_t value, MemoryOperationType type);
	void ProcessSpcWrite(uint16_t addr, uint8_t value, MemoryOperationType type);

	void ProcessPpuRead(uint16_t addr, uint8_t value, SnesMemoryType memoryType);
	void ProcessPpuWrite(uint16_t addr, uint8_t value, SnesMemoryType memoryType);
	void ProcessPpuCycle();

	void ProcessNecDspExec(uint32_t addr, uint32_t value);

	void ProcessInterrupt(uint32_t originalPc, uint32_t currentPc, bool forNmi);
	void ProcessEvent(EventType type);

	int32_t EvaluateExpression(string expression, CpuType cpuType, EvalResultType &resultType, bool useCache);

	void Run();
	void Step(int32_t stepCount, StepType type = StepType::CpuStep);
	bool IsExecutionStopped();

	void BreakRequest(bool release);
	void SuspendDebugger(bool release);

	void GetState(DebugState &state, bool partialPpuState);

	AddressInfo GetAbsoluteAddress(AddressInfo relAddress);
	AddressInfo GetRelativeAddress(AddressInfo absAddress);

	void SetCdlData(uint8_t * cdlData, uint32_t length);
	void RefreshCodeCache();

	shared_ptr<TraceLogger> GetTraceLogger();
	shared_ptr<MemoryDumper> GetMemoryDumper();
	shared_ptr<MemoryAccessCounter> GetMemoryAccessCounter();
	shared_ptr<CodeDataLogger> GetCodeDataLogger();
	shared_ptr<Disassembler> GetDisassembler();
	shared_ptr<BreakpointManager> GetBreakpointManager();
	shared_ptr<PpuTools> GetPpuTools();
	shared_ptr<EventManager> GetEventManager();
	shared_ptr<LabelManager> GetLabelManager();
	shared_ptr<ScriptManager> GetScriptManager();
	shared_ptr<CallstackManager> GetCallstackManager(CpuType cpuType);
	shared_ptr<Console> GetConsole();
};