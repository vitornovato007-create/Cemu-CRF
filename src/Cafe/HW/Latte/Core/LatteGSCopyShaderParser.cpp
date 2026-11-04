#include "Cafe/HW/Latte/Core/LatteConst.h"
#include "Cafe/HW/Latte/Core/LatteShaderAssembly.h"
#include "Cafe/HW/Latte/LegacyShaderDecompiler/LatteDecompiler.h"
#include <cstring>

bool LatteGSCopyShaderParser_addFetchedParam(LatteParsedGSCopyShader* shaderContext, uint32 offset, uint32 gprIndex)
{
	if (shaderContext->numParam >= GPU7_COPY_SHADER_MAX_PARAMS)
	{
		cemuLog_log(LogType::Force, "Copy shader has too many fetched parameters: limit={}, offset={}, gpr={}", GPU7_COPY_SHADER_MAX_PARAMS, offset, gprIndex);
		return false;
	}
	shaderContext->paramMapping[shaderContext->numParam].exportParam = 0xFF;
	shaderContext->paramMapping[shaderContext->numParam].offset = offset;
	shaderContext->paramMapping[shaderContext->numParam].gprIndex = gprIndex;
	shaderContext->numParam++;
	return true;
}

bool LatteGSCopyShaderParser_assignRegisterParameterOutput(LatteParsedGSCopyShader* shaderContext, uint32 gprIndex, uint32 exportType, uint32 exportParam)
{
	if (exportParam >= 0x100 || (exportType == 2 && exportParam >= 16))
	{
		cemuLog_log(LogType::Force, "Copy shader has unsupported export parameter: gpr={}, exportType={}, exportParam={}", gprIndex, exportType, exportParam);
		return false;
	}
	// scan backwards to catch the most recently added entry in case a register has multiple entries
	for (sint32 i = shaderContext->numParam - 1; i >= 0; i--)
	{
		if (shaderContext->paramMapping[i].gprIndex == gprIndex)
		{
			if (shaderContext->paramMapping[i].exportParam != 0xFF)
			{
				cemuLog_log(LogType::Force, "Copy shader register {} receives multiple export assignments", gprIndex);
				return false;
			}
			shaderContext->paramMapping[i].exportType = (uint8)exportType;
			shaderContext->paramMapping[i].exportParam = (uint8)exportParam;
			return true;
		}
	}
	cemuLog_log(LogType::Force, "Copy shader exports an uninitialized register: gpr={}, exportType={}, exportParam={}", gprIndex, exportType, exportParam);
	return false;
}

bool LatteGSCopyShaderParser_addStreamWrite(LatteParsedGSCopyShader* shaderContext, uint32 bufferIndex, uint32 exportSourceGPR, uint32 exportArrayBase, uint32 memWriteArraySize, uint32 memWriteCompMask)
{
	// get info about current state of GPR
	for (sint32 i = shaderContext->numParam - 1; i >= 0; i--)
	{
		if (shaderContext->paramMapping[i].gprIndex == exportSourceGPR)
		{
			LatteGSCopyShaderStreamWrite_t streamWrite;
			streamWrite.bufferIndex = (uint8)bufferIndex;
			streamWrite.offset = shaderContext->paramMapping[i].offset;
			streamWrite.exportArrayBase = exportArrayBase;
			streamWrite.memWriteArraySize = memWriteArraySize;
			streamWrite.memWriteCompMask = memWriteCompMask;
			shaderContext->list_streamWrites.push_back(streamWrite);
			return true;
		}
	}
	cemuLog_log(LogType::Force, "Copy shader stream write references an uninitialized register: gpr={}, buffer={}", exportSourceGPR, bufferIndex);
	return false;
}

bool LatteGSCopyShaderParser_getExportTypeByOffset(LatteParsedGSCopyShader* shaderContext, uint32 offset, uint32* exportType, uint32* exportParam)
{
	for(sint32 i=0; i<shaderContext->numParam; i++)
	{
		if( shaderContext->paramMapping[i].offset == offset )
		{
			*exportType = shaderContext->paramMapping[i].exportType;
			*exportParam = shaderContext->paramMapping[i].exportParam;
			return true;
		}
	}
	return false;
}

bool LatteGSCopyShaderParser_parseClauseVtx(LatteParsedGSCopyShader* shaderContext, uint8* programData, uint32 programSize, uint32 addr, uint32 count)
{
	for(uint32 i=0; i<count; i++)
	{
		const uint64 instructionOffset = ((uint64)addr * 2 + (uint64)i * 4) * sizeof(uint32);
		if (instructionOffset > programSize || programSize - instructionOffset < 4 * sizeof(uint32))
		{
			cemuLog_log(LogType::Force, "Copy shader VTX clause exceeds program bounds: addr={}, instruction={}, byteOffset={}, programBytes={}", addr, i, instructionOffset, programSize);
			return false;
		}
		uint32 word0, word1, word2, word3;
		const uint8* instructionData = programData + instructionOffset;
		std::memcpy(&word0, instructionData + 0, sizeof(uint32));
		std::memcpy(&word1, instructionData + 4, sizeof(uint32));
		std::memcpy(&word2, instructionData + 8, sizeof(uint32));
		std::memcpy(&word3, instructionData + 12, sizeof(uint32));
		uint32 inst0_4 = (word0>>0)&0x1F;
		if( inst0_4 == GPU7_TEX_INST_VFETCH )
		{
			// data fetch
			uint32 fetchType = (word0>>5)&3;
			uint32 bufferId = (word0>>8)&0xFF;
			uint32 offset = (word2>>0)&0xFFFF;
			uint32 endianSwap = (word2>>16)&0x3;
			uint32 constNoStride = (word2>>18)&0x1;
			uint32 srcGpr = (word0>>16)&0x7F;
			uint32 srcRel = (word0>>23)&1;
			if( srcRel != 0 )
				debugBreakpoint();
			uint32 destGpr = (word1>>0)&0x7F;
			uint32 destRel = (word1>>7)&1;
			if( destRel != 0 )
				debugBreakpoint();
			uint32 dstSelX = (word1>>9)&0x7;
			uint32 dstSelY = (word1>>12)&0x7;
			uint32 dstSelZ = (word1>>15)&0x7;
			uint32 dstSelW = (word1>>18)&0x7;

			uint32 srcSelX = (word0>>24)&0x3;
			uint32 srcSelY = 0;
			uint32 srcSelZ = 0;
			uint32 srcSelW = 0;

			if( bufferId != 0x9F )
			{
				debugBreakpoint(); // data not fetched from GS ring buffer
				return false;
			}
			if( endianSwap != 0 )
				debugBreakpoint();
			if( fetchType != 2 )
				debugBreakpoint();
			if( srcSelX != 0 || srcGpr != 0 )
				debugBreakpoint();
			if( dstSelX != 0 || dstSelY != 1 || dstSelZ != 2 || dstSelW != 3 )
				debugBreakpoint();
			// remember imported parameter
			if (!LatteGSCopyShaderParser_addFetchedParam(shaderContext, offset, destGpr))
				return false;
		}
		else
		{
			return false;
		}
	}
	return true;
}

LatteParsedGSCopyShader* LatteGSCopyShaderParser_parse(uint8* programData, uint32 programSize)
{
	if (programData == nullptr || programSize < 8 || (programSize & 3) != 0)
	{
		cemuLog_log(LogType::Force, "Copy shader parser received invalid program buffer: dataNull={}, programBytes={}", programData == nullptr, programSize);
		return nullptr;
	}
	LatteParsedGSCopyShader* shaderContext = new LatteParsedGSCopyShader();
	shaderContext->numParam = 0;
	// parse control flow instructions
	for(uint32 i=0; i<programSize/8; i++)
	{
		uint32 cfWord0, cfWord1;
		std::memcpy(&cfWord0, programData + (size_t)i * 8, sizeof(uint32));
		std::memcpy(&cfWord1, programData + (size_t)i * 8 + 4, sizeof(uint32));
		uint32 cf_inst23_7 = (cfWord1>>23)&0x7F;
		// check the bigger opcode fields first
		if( cf_inst23_7 < 0x40 ) // at 0x40 the bits overlap with the ALU instruction encoding
		{
			bool isEndOfProgram = ((cfWord1>>21)&1)!=0;
			uint32 addr = cfWord0&0xFFFFFFFF;
			uint32 count = (cfWord1>>10)&7;
			if( ((cfWord1>>19)&1) != 0 )
				count |= 0x8;
			count++;
			if( cf_inst23_7 == GPU7_CF_INST_CALL_FS )
			{
				// nop
			}
			else if( cf_inst23_7 == GPU7_CF_INST_NOP )
			{
				// nop
				if( ((cfWord1>>0)&7) != 0 )
					debugBreakpoint(); // pop count is not zero, 
			}
			else if( cf_inst23_7 == GPU7_CF_INST_EXPORT || cf_inst23_7 == GPU7_CF_INST_EXPORT_DONE )
			{
				// export
				uint32 edType = (cfWord0>>13)&0x3;
				uint32 edIndexGpr = (cfWord0>>23)&0x7F;
				uint32 edRWRel = (cfWord0>>22)&1;
				if( edRWRel != 0 || edIndexGpr != 0 )
					debugBreakpoint();
				// set export component selection
				uint8 exportComponentSel[4];
				exportComponentSel[0] = (cfWord1>>0)&0x7;
				exportComponentSel[1] = (cfWord1>>3)&0x7;
				exportComponentSel[2] = (cfWord1>>6)&0x7;
				exportComponentSel[3] = (cfWord1>>9)&0x7;
				// set export array base, index and burstcount (export field)
				uint32 exportArrayBase = (cfWord0>>0)&0x1FFF;
				uint32 exportBurstCount = (cfWord1>>17)&0xF;
				// set export source GPR and type
				uint32 exportSourceGPR = (cfWord0>>15)&0x7F;
				uint32 exportType = edType;
				if (exportArrayBase == GPU7_DECOMPILER_CF_EXPORT_BASE_POSITION && exportComponentSel[0] == 4 && exportComponentSel[1] == 4 && exportComponentSel[2] == 4 && exportComponentSel[3] == 4)
				{
					// aka gl_Position = vec4(0.0)
					// this instruction form is generated when the original shader doesn't assign gl_Position a value?
				}
				else if (exportComponentSel[0] != 0 || exportComponentSel[1] != 1 || exportComponentSel[2] != 2 || exportComponentSel[3] != 3)
				{
					cemu_assert_debug(false);
				}
				else
				{
					// register as param
					for (uint32 f = 0; f < exportBurstCount + 1; f++)
					{
						if (!LatteGSCopyShaderParser_assignRegisterParameterOutput(shaderContext, exportSourceGPR + f, exportType, exportArrayBase + f))
						{
							delete shaderContext;
						return nullptr;
						}
					}
				}
			}
			else if( cf_inst23_7 == GPU7_CF_INST_VTX )
			{
				if (!LatteGSCopyShaderParser_parseClauseVtx(shaderContext, programData, programSize, addr, count))
				{
					cemuLog_log(LogType::Force, "Copy shader VTX clause could not be parsed: addr={}, count={}, programBytes={}", addr, count, programSize);
					delete shaderContext;
					return nullptr;
				}
			}
			else if (cf_inst23_7 == GPU7_CF_INST_MEM_STREAM0_WRITE ||
				cf_inst23_7 == GPU7_CF_INST_MEM_STREAM1_WRITE )
			{
				// streamout
				uint32 bufferIndex;
				if (cf_inst23_7 == GPU7_CF_INST_MEM_STREAM0_WRITE)
					bufferIndex = 0;
				else if (cf_inst23_7 == GPU7_CF_INST_MEM_STREAM1_WRITE)
					bufferIndex = 1;
				else
					cemu_assert_debug(false);

				uint32 exportArrayBase = (cfWord0 >> 0) & 0x1FFF;
				uint32 memWriteArraySize = (cfWord1 >> 0) & 0xFFF;
				uint32 memWriteCompMask = (cfWord1 >> 12) & 0xF;
				uint32 exportSourceGPR = (cfWord0 >> 15) & 0x7F;

				if (!LatteGSCopyShaderParser_addStreamWrite(shaderContext, bufferIndex, exportSourceGPR, exportArrayBase, memWriteArraySize, memWriteCompMask))
				{
					delete shaderContext;
					return nullptr;
				}
			}
			else
			{
				cemuLog_log(LogType::Force, "Copyshader: Unknown 23_7 clause 0x{:x} found", cf_inst23_7);
				cemu_assert_debug(false);
			}
			if( isEndOfProgram )
			{
				break;
			}
		}
		else
		{
			// ALU clauses not supported
			debug_printf("Copyshader has ALU clause?\n");
			cemu_assert_debug(false);
			delete shaderContext;
			return nullptr;
		}
	}
	// verify if all registers are exported
	for(sint32 i=0; i<shaderContext->numParam; i++)
	{
		if( shaderContext->paramMapping[i].exportParam == 0xFF )
			debugBreakpoint();
	}
	return shaderContext;
}
