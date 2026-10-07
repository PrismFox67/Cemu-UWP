#include "Cafe/HW/Latte/Renderer/D3D12/D3D12BindingModel.h"

#include <algorithm>
#include <unordered_map>
#include <unordered_set>

namespace
{
	// --- SPIR-V parsing helpers ---
	namespace spirv_ops
	{
		constexpr uint32 MagicNumber = 0x07230203;
		constexpr uint32 OpName = 5;
		constexpr uint32 OpTypeImage = 25;
		constexpr uint32 OpTypeSampler = 26;
		constexpr uint32 OpTypeSampledImage = 27;
		constexpr uint32 OpTypeArray = 28;
		constexpr uint32 OpTypeRuntimeArray = 29;
		constexpr uint32 OpTypeStruct = 30;
		constexpr uint32 OpTypePointer = 32;
		constexpr uint32 OpVariable = 59;
		constexpr uint32 OpDecorate = 71;
		constexpr uint32 OpMemberDecorate = 72;
		constexpr uint32 OpDecorationGroup = 73;
		constexpr uint32 OpGroupDecorate = 74;
		constexpr uint32 OpGroupMemberDecorate = 75;
		constexpr uint32 OpDecorateId = 332;
		constexpr uint32 OpDecorateString = 5632;
		constexpr uint32 OpMemberDecorateString = 5633;

		constexpr uint32 DecorationBlock = 2;
		constexpr uint32 DecorationBufferBlock = 3;
		constexpr uint32 DecorationBinding = 33;
		constexpr uint32 DecorationDescriptorSet = 34;

		constexpr uint32 StorageClassUniformConstant = 0;
		constexpr uint32 StorageClassUniform = 2;
		constexpr uint32 StorageClassStorageBuffer = 12;

		bool IsAnnotation(uint32 op)
		{
			return op == OpDecorate || op == OpMemberDecorate || op == OpDecorationGroup || op == OpGroupDecorate ||
				op == OpGroupMemberDecorate || op == OpDecorateId || op == OpDecorateString || op == OpMemberDecorateString;
		}
	}
}

bool D3D12_RemapSpirvBindings(std::vector<uint32>& spirv, D3D12Const::Stage stage, D3D12BindingRemap& remapOut, std::string& logOut)
{
	remapOut = D3D12BindingRemap();
	if (spirv.size() < 5 || spirv[0] != spirv_ops::MagicNumber)
	{
		logOut = "invalid SPIR-V module";
		return false;
	}

	struct DecorationInfo
	{
		sint64 bindingWord = -1; // index of the literal within the module
		sint64 setWord = -1;
		uint32 binding = 0;
		uint32 set = 0;
	};
	struct TypeInfo
	{
		uint32 op = 0;
		uint32 operand0 = 0; // pointer: pointee, array: element type, sampled image: image type, image: 'sampled' operand
		uint32 storageClass = 0; // pointers only
	};
	struct VariableInfo
	{
		uint32 id;
		uint32 pointerType;
		uint32 storageClass;
	};

	std::unordered_map<uint32, DecorationInfo> decorations;
	std::unordered_set<uint32> blockTypes;
	std::unordered_set<uint32> bufferBlockTypes;
	std::unordered_map<uint32, TypeInfo> types;
	std::vector<VariableInfo> variables;
	size_t firstAnnotationWord = 0;
	size_t firstTypeWord = 0;

	size_t pos = 5;
	while (pos < spirv.size())
	{
		const uint32 wordCount = spirv[pos] >> 16;
		const uint32 op = spirv[pos] & 0xFFFF;
		if (wordCount == 0 || pos + wordCount > spirv.size())
		{
			logOut = "malformed SPIR-V instruction stream";
			return false;
		}
		const uint32* w = spirv.data() + pos;
		if (spirv_ops::IsAnnotation(op) && firstAnnotationWord == 0)
			firstAnnotationWord = pos;
		switch (op)
		{
		case spirv_ops::OpDecorate:
			if (wordCount >= 3)
			{
				uint32 target = w[1];
				uint32 decoration = w[2];
				if (decoration == spirv_ops::DecorationBinding && wordCount >= 4)
				{
					decorations[target].bindingWord = (sint64)(pos + 3);
					decorations[target].binding = w[3];
				}
				else if (decoration == spirv_ops::DecorationDescriptorSet && wordCount >= 4)
				{
					decorations[target].setWord = (sint64)(pos + 3);
					decorations[target].set = w[3];
				}
				else if (decoration == spirv_ops::DecorationBlock)
					blockTypes.insert(target);
				else if (decoration == spirv_ops::DecorationBufferBlock)
					bufferBlockTypes.insert(target);
			}
			break;
		case spirv_ops::OpTypeImage:
			if (firstTypeWord == 0)
				firstTypeWord = pos;
			if (wordCount >= 8)
				types[w[1]] = { op, w[7], 0 }; // operand 'Sampled' (1 = sampled, 2 = storage)
			break;
		case spirv_ops::OpTypeSampler:
			if (firstTypeWord == 0)
				firstTypeWord = pos;
			types[w[1]] = { op, 0, 0 };
			break;
		case spirv_ops::OpTypeSampledImage:
		case spirv_ops::OpTypeArray:
		case spirv_ops::OpTypeRuntimeArray:
			if (firstTypeWord == 0)
				firstTypeWord = pos;
			if (wordCount >= 3)
				types[w[1]] = { op, w[2], 0 };
			break;
		case spirv_ops::OpTypeStruct:
			if (firstTypeWord == 0)
				firstTypeWord = pos;
			types[w[1]] = { op, 0, 0 };
			break;
		case spirv_ops::OpTypePointer:
			if (firstTypeWord == 0)
				firstTypeWord = pos;
			if (wordCount >= 4)
				types[w[1]] = { op, w[3], w[2] };
			break;
		case spirv_ops::OpVariable:
			if (wordCount >= 4)
				variables.push_back({ w[2], w[1], w[3] });
			break;
		default:
			break;
		}
		pos += wordCount;
	}

	auto resolveBaseType = [&](uint32 typeId) -> const TypeInfo* {
		// strip arrays
		for (int depth = 0; depth < 8; depth++)
		{
			auto it = types.find(typeId);
			if (it == types.end())
				return nullptr;
			if (it->second.op == spirv_ops::OpTypeArray || it->second.op == spirv_ops::OpTypeRuntimeArray)
			{
				typeId = it->second.operand0;
				continue;
			}
			return &it->second;
		}
		return nullptr;
	};

	struct Resource
	{
		uint32 varId;
		uint32 originalSet;
		uint32 originalBinding;
	};
	std::vector<Resource> perClass[3];

	for (auto& var : variables)
	{
		auto ptrIt = types.find(var.pointerType);
		if (ptrIt == types.end() || ptrIt->second.op != spirv_ops::OpTypePointer)
			continue;
		uint32 pointeeId = ptrIt->second.operand0;
		const TypeInfo* base = resolveBaseType(pointeeId);
		uint32 baseId = pointeeId;
		// resolve base type id for block decoration lookups
		for (int depth = 0; depth < 8; depth++)
		{
			auto it = types.find(baseId);
			if (it == types.end() || (it->second.op != spirv_ops::OpTypeArray && it->second.op != spirv_ops::OpTypeRuntimeArray))
				break;
			baseId = it->second.operand0;
		}
		if (!base)
			continue;
		sint32 cls = -1;
		if (var.storageClass == spirv_ops::StorageClassUniformConstant)
		{
			if (base->op == spirv_ops::OpTypeSampledImage || base->op == spirv_ops::OpTypeSampler)
				cls = (sint32)D3D12Const::BindingClass::SRV;
			else if (base->op == spirv_ops::OpTypeImage)
				cls = (base->operand0 == 2) ? (sint32)D3D12Const::BindingClass::UAV : (sint32)D3D12Const::BindingClass::SRV;
		}
		else if (var.storageClass == spirv_ops::StorageClassUniform)
		{
			if (bufferBlockTypes.count(baseId))
				cls = (sint32)D3D12Const::BindingClass::UAV;
			else
				cls = (sint32)D3D12Const::BindingClass::CBV;
		}
		else if (var.storageClass == spirv_ops::StorageClassStorageBuffer)
			cls = (sint32)D3D12Const::BindingClass::UAV;
		if (cls < 0)
			continue; // inputs, outputs, push constants etc.
		auto decIt = decorations.find(var.id);
		if (decIt == decorations.end() || decIt->second.bindingWord < 0)
		{
			logOut = fmt::format("resource variable %{} has no binding decoration", var.id);
			return false;
		}
		if (decIt->second.binding >= D3D12BindingRemap::kMaxBinding)
		{
			logOut = fmt::format("binding {} exceeds the supported maximum of {}", decIt->second.binding, D3D12BindingRemap::kMaxBinding - 1);
			return false;
		}
		perClass[cls].push_back({ var.id, decIt->second.set, decIt->second.binding });
	}

	std::vector<uint32> insertedDecorations;
	for (uint32 cls = 0; cls < 3; cls++)
	{
		auto& list = perClass[cls];
		std::sort(list.begin(), list.end(), [](const Resource& a, const Resource& b) {
			if (a.originalBinding != b.originalBinding)
				return a.originalBinding < b.originalBinding;
			return a.originalSet < b.originalSet;
		});
		const uint32 space = D3D12Const::RegisterSpace(stage, (D3D12Const::BindingClass)cls);
		sint8* remapTable = (cls == 0) ? remapOut.cbv : (cls == 1) ? remapOut.srv : remapOut.uav;
		uint32 rank = 0;
		for (auto& res : list)
		{
			// two variables can share a binding when they live in different descriptor sets. Only the shader's own
			// set is meaningful to Cemu, so treat duplicates as an error instead of silently aliasing
			if (remapTable[res.originalBinding] != D3D12BindingRemap::kUnused)
			{
				logOut = fmt::format("binding {} is used by more than one resource of the same class", res.originalBinding);
				return false;
			}
			remapTable[res.originalBinding] = (sint8)rank;
			DecorationInfo& dec = decorations[res.varId];
			spirv[(size_t)dec.bindingWord] = rank;
			if (dec.setWord >= 0)
				spirv[(size_t)dec.setWord] = space;
			else
			{
				insertedDecorations.push_back((4u << 16) | spirv_ops::OpDecorate);
				insertedDecorations.push_back(res.varId);
				insertedDecorations.push_back(spirv_ops::DecorationDescriptorSet);
				insertedDecorations.push_back(space);
			}
			rank++;
		}
		if (cls == 0)
			remapOut.cbvCount = (uint8)rank;
		else if (cls == 1)
			remapOut.srvCount = (uint8)rank;
		else
			remapOut.uavCount = (uint8)rank;
	}
	if (remapOut.cbvCount > D3D12Const::kMaxCBVsPerStage || remapOut.srvCount > D3D12Const::kMaxSRVsPerStage || remapOut.uavCount > D3D12Const::kMaxUAVsPerStage)
	{
		logOut = fmt::format("shader uses too many resources (CBV {} SRV {} UAV {})", remapOut.cbvCount, remapOut.srvCount, remapOut.uavCount);
		return false;
	}
	if (!insertedDecorations.empty())
	{
		size_t insertAt = firstAnnotationWord ? firstAnnotationWord : firstTypeWord;
		if (insertAt == 0)
		{
			logOut = "unable to find insertion point for decorations";
			return false;
		}
		spirv.insert(spirv.begin() + insertAt, insertedDecorations.begin(), insertedDecorations.end());
	}
	return true;
}

