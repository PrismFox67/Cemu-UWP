#include "Cafe/HW/Latte/Core/LatteConst.h"
#include "Cafe/HW/Latte/Core/LatteShaderAssembly.h"
#include "Cafe/HW/Latte/LegacyShaderDecompiler/LatteDecompilerInstructions.h"
#include "Cafe/HW/Latte/LegacyShaderDecompiler/LatteDecompiler.h"
#include "Cafe/HW/Latte/LegacyShaderDecompiler/LatteDecompilerInternal.h"
#include "Cafe/HW/Latte/ISA/LatteReg.h"

// #449, adapted from danprice142/Cemu-UWP (MPL-2.0, LatteDecompilerEmitHLSL.cpp CountIndexedGPRWriteSelects and
// GetIndexedGPRLimit at b68b069). A lowered shader gets one select per relative write and candidate register
static void _CountIndexedGPRWriteSelects(LatteDecompilerShaderContext* shaderContext, uint32& writesOut, uint32& registersOut)
{
	uint32 writes = 0;
	const auto countWrites = [&writes](const std::vector<LatteDecompilerCFInstruction>& cfInstructions)
	{
		for (const auto& cfInstruction : cfInstructions)
		{
			for (const auto& aluInstruction : cfInstruction.instructionsALU)
			{
				if (aluInstruction.destRel != 0 && aluInstruction.writeMask != 0)
					writes++;
			}
		}
	};
	countWrites(shaderContext->cfInstructions);
	for (const auto& subroutineInfo : shaderContext->list_subroutines)
		countWrites(subroutineInfo.instructions);
	uint32 registers = 0;
	for (sint32 i = 0; i < (sint32)shaderContext->indexedGPRLimit; i++)
	{
		if (shaderContext->analyzer.usesRelativeGPRRead || (shaderContext->analyzer.gprUseMask[i / 8] & (1 << (i % 8))) != 0)
			registers++;
	}
	writesOut = writes;
	registersOut = registers;
}

// a program cannot address registers beyond the ones it allocates
static uint32 _GetIndexedGPRLimit(const LatteDecompilerShaderContext* shaderContext)
{
	const LatteContextRegister& registers = *shaderContext->contextRegistersNew;
	uint32 allocated;
	if (shaderContext->shaderType == LatteConst::ShaderType::Pixel)
		allocated = registers.SQ_PGM_RESOURCES_PS.get_NUM_GPRS();
	else if (shaderContext->shaderType == LatteConst::ShaderType::Geometry)
		allocated = registers.SQ_PGM_RESOURCES_GS.get_NUM_GPRS();
	else if (shaderContext->options->usesGeometryShader)
		allocated = registers.SQ_PGM_RESOURCES_ES.get_NUM_GPRS(); // a vertex shader feeding a geometry shader runs as the export shader
	else
		allocated = registers.SQ_PGM_RESOURCES_VS.get_NUM_GPRS();
	return allocated == 0 ? LATTE_NUM_GPR : std::min<uint32>(allocated, LATTE_NUM_GPR);
}

void LatteDecompiler_analyzeDataTypes(LatteDecompilerShaderContext* shaderContext)
{
	// determine default type
	if (shaderContext->analyzer.usesIntegerValues)
	{
		shaderContext->typeTracker.defaultDataType = LATTE_DECOMPILER_DTYPE_SIGNED_INT;
		shaderContext->typeTracker.genIntReg = true;
	}
	else
	{
		shaderContext->typeTracker.defaultDataType = LATTE_DECOMPILER_DTYPE_FLOAT;
		shaderContext->typeTracker.genFloatReg = true;
	}
	// determine register representation
	if (shaderContext->analyzer.usesRelativeGPRWrite)
	{
		shaderContext->typeTracker.useArrayGPRs = true;
		if (shaderContext->options->lowerRelativeGPRWrites)
		{
			// #449: the Xbox driver loses what a pixel shader stores in an indexed register array (BOTW's Magnesis pick
			// shader 3220798430eaf1bb). Relative writes become selects over register variables, bounded by the registers
			// the program allocates; very large shaders keep the array
			shaderContext->indexedGPRLimit = _GetIndexedGPRLimit(shaderContext);
			constexpr uint32 kMaxIndexedGPRWriteSelects = 16384;
			uint32 writes, registers;
			_CountIndexedGPRWriteSelects(shaderContext, writes, registers);
			if (writes * registers <= kMaxIndexedGPRWriteSelects)
			{
				shaderContext->typeTracker.useArrayGPRs = false;
				shaderContext->lowerIndexedGPRWrites = true;
				cemuLog_log(LogType::Force, "D3D12: shader {:016x} lowers {} indexed GPR writes over {} registers", shaderContext->shaderBaseHash, writes, registers);
			}
			else
			{
				cemuLog_log(LogType::Force, "D3D12: shader {:016x} keeps the register array: {} indexed GPR writes over {} registers",
					shaderContext->shaderBaseHash, writes, registers);
			}
		}
	}
}