#include "graphics/shader/recompiler/Tessellation.h"

#include "common/assert.h"
#include "common/logging/log.h"
#include "graphics/shader/recompiler/ShaderRecompiler.h"
#include "graphics/shader/recompiler/frontend/decode/ShaderDecoder.h"
#include "graphics/shader/recompiler/ir/ShaderIR.h"
#include "graphics/shader/recompiler/ir/passes/ConstantPropagation.h"
#include "graphics/shader/recompiler/ir/passes/DeadCodeElimination.h"

#include <algorithm>
#include <array>
#include <unordered_map>
#include <utility>

namespace Libs::Graphics::ShaderRecompiler {
namespace {

struct TessellationAddress {
	enum class Kind { Unknown, Affine, PackedControlPoint };
	Kind     kind        = Kind::Unknown;
	uint32_t coefficient = 0;
	uint32_t constant    = 0;
};

uint32_t ReflectTessellationStride(const Decoder::Program& program, bool local,
                                   const ShaderTessellationInputInfo& info) {
	using namespace Decoder;
	using Address = TessellationAddress;
	using Kind    = Address::Kind;
	const auto control_points = local ? info.input_control_points : info.output_control_points;
	const auto input_stride   = local ? 0u : info.ls_stride;
	std::array<Address, IR::NumVectorRegs> registers {};
	std::array<Address, IR::NumScalarRegs> scalars {};
	Address                                vcc {};
	if (local) {
		registers[3] = Address {Kind::Affine, 1u, 0u};
	} else {
		// Native HS v0 is patch ID and v1 packs patch ordinal plus invocation.
		// The logical interface is one patch, matching Translate.cpp.
		registers[0] = Address {Kind::Affine, 0u, 0u};
		registers[1] = Address {Kind::PackedControlPoint};
		scalars[2]   = Address {Kind::Affine, 0u, 0u};
		scalars[3]   = Address {Kind::Affine, 0u,
		                        0x81010000u | info.input_control_points |
		                            (info.output_control_points << 8u)};
		scalars[4]   = Address {Kind::Affine, 0u, 0u};
	}
	const auto constant = [](uint32_t value) { return Address {Kind::Affine, 0u, value}; };
	const auto affine   = [](uint64_t coefficient, uint64_t offset) {
		return coefficient <= UINT32_MAX && offset <= UINT32_MAX
		           ? Address {Kind::Affine, static_cast<uint32_t>(coefficient),
		                      static_cast<uint32_t>(offset)}
		           : Address {};
	};
	const auto apply_sdwa = [&](Address value, uint32_t sel) -> Address {
		if (sel == 6u) return value;
		if (value.kind == Kind::PackedControlPoint) {
			if (sel == 0u) return constant(0u);
			if (sel == 1u) return Address {Kind::Affine, 1u, 0u};
			return {};
		}
		return value.kind == Kind::Affine && value.coefficient == 0u && sel == 0u
		           ? constant(value.constant & 0xffu)
		           : Address {};
	};
	const auto read = [&](const Operand& operand) -> Address {
		if (operand.absolute || operand.negate || operand.sdwa_sext || operand.op_sel ||
		    operand.op_sel_hi || operand.negate_hi || operand.dpp)
			return {};
		Address value;
		if (operand.kind == OperandKind::IntegerInlineConstant ||
		    operand.kind == OperandKind::LiteralConstant) {
			value = constant(operand.value);
		} else if (operand.kind == OperandKind::Vgpr && operand.reg < registers.size()) {
			value = registers[operand.reg];
		} else if (operand.kind == OperandKind::Sgpr && operand.reg < scalars.size()) {
			value = scalars[operand.reg];
		} else if (operand.kind == OperandKind::VccLo) {
			value = vcc;
		}
		return apply_sdwa(value, operand.sdwa_sel);
	};
	const auto add = [&](Address lhs, Address rhs) {
		return lhs.kind == Kind::Affine && rhs.kind == Kind::Affine
		           ? affine(uint64_t {lhs.coefficient} + rhs.coefficient,
		                    uint64_t {lhs.constant} + rhs.constant)
		           : Address {};
	};
	const auto subtract = [&](Address lhs, Address rhs) {
		return lhs.kind == Kind::Affine && rhs.kind == Kind::Affine &&
		               lhs.coefficient >= rhs.coefficient && lhs.constant >= rhs.constant
		           ? affine(lhs.coefficient - rhs.coefficient, lhs.constant - rhs.constant)
		           : Address {};
	};
	const auto multiply = [&](Address lhs, Address rhs) {
		if ((lhs.kind == Kind::Affine && lhs.coefficient == 0u && lhs.constant == 0u) ||
		    (rhs.kind == Kind::Affine && rhs.coefficient == 0u && rhs.constant == 0u))
			return constant(0u);
		if (lhs.kind != Kind::Affine || rhs.kind != Kind::Affine) return Address {};
		if (rhs.coefficient != 0u) std::swap(lhs, rhs);
		return rhs.coefficient == 0u ? affine(uint64_t {lhs.coefficient} * rhs.constant,
		                                      uint64_t {lhs.constant} * rhs.constant)
		                             : Address {};
	};
	const auto low24 = [&](Address value) {
		if (value.kind == Kind::Affine && value.coefficient == 0u)
			return constant(value.constant & 0xffffffu);
		return value.kind == Kind::Affine &&
		               uint64_t {value.coefficient} * (control_points - 1u) + value.constant <=
		                   0xffffffu
		           ? value
		           : Address {};
	};
	const auto shift_left = [&](Address value, Address amount) {
		return amount.kind == Kind::Affine && amount.coefficient == 0u && amount.constant < 32u
		           ? multiply(value, constant(1u << amount.constant))
		           : Address {};
	};
	const auto shift_right = [&](Address value, Address amount) {
		if (amount.kind != Kind::Affine || amount.coefficient != 0u || amount.constant >= 32u)
			return Address {};
		const auto bits = amount.constant;
		if (value.kind == Kind::PackedControlPoint) {
			return bits == 8u ? Address {Kind::Affine, 1u, 0u} : Address {};
		}
		if (value.kind != Kind::Affine) return Address {};
		if (value.coefficient == 0u) return constant(value.constant >> bits);
		const uint32_t divisor = 1u << bits;
		return (value.coefficient % divisor) == 0u && (value.constant % divisor) == 0u
		           ? affine(value.coefficient / divisor, value.constant / divisor)
		           : Address {};
	};
	const auto bitwise_and = [&](Address lhs, Address rhs) {
		if (rhs.kind != Kind::Affine || rhs.coefficient != 0u) std::swap(lhs, rhs);
		if (rhs.kind != Kind::Affine || rhs.coefficient != 0u) return Address {};
		const auto mask = rhs.constant;
		if (mask == 0u) return constant(0u);
		if (lhs.kind == Kind::PackedControlPoint) {
			if ((mask & ~0xffu) == 0u) return constant(0u);
			if ((mask & ~0xff00u) == 0u) return Address {Kind::Affine, 256u, 0u};
			return Address {};
		}
		if (lhs.kind != Kind::Affine) return Address {};
		if (lhs.coefficient == 0u) return constant(lhs.constant & mask);
		const uint64_t max_value =
		    uint64_t {lhs.coefficient} * (control_points - 1u) + lhs.constant;
		return (max_value & ~uint64_t {mask}) == 0u ? lhs : Address {};
	};
	const auto bit_extract = [&](Address value, Address offset, Address width) {
		if (offset.kind != Kind::Affine || offset.coefficient != 0u ||
		    width.kind != Kind::Affine || width.coefficient != 0u)
			return Address {};
		if (value.kind == Kind::PackedControlPoint) {
			if (offset.constant == 0u && width.constant <= 8u) return constant(0u);
			if (offset.constant == 8u && width.constant >= 5u && width.constant <= 8u)
				return Address {Kind::Affine, 1u, 0u};
			return Address {};
		}
		if (value.kind == Kind::Affine && value.coefficient == 0u && offset.constant < 32u &&
		    width.constant <= 32u) {
			const auto mask = width.constant == 32u ? 0xffffffffu : (1u << width.constant) - 1u;
			return constant((value.constant >> offset.constant) & mask);
		}
		return Address {};
	};
	const auto define = [&](const Operand& dst, Address value, uint32_t data_dwords) {
		if (dst.sdwa_sel != 6u || dst.op_sel || dst.omod || dst.clamp || dst.dpp) value = {};
		if (dst.kind == OperandKind::Vgpr && dst.reg < registers.size()) {
			for (uint32_t index = 0;
			     index < std::max(data_dwords, 1u) && dst.reg + index < registers.size(); index++) {
				registers[dst.reg + index] = {};
			}
			registers[dst.reg] = value;
		} else if (dst.kind == OperandKind::Sgpr && dst.reg < scalars.size()) {
			scalars[dst.reg] = value;
		} else if (dst.kind == OperandKind::VccLo) {
			vcc = value;
		}
	};
	uint32_t stride = 0;
	for (const auto& inst: program.instructions) {
		// Stage exits preserve the active path's definitions. An internal join
		// would require merging register definitions.
		EXIT_NOT_IMPLEMENTED(IsDirectBranch(inst.opcode) &&
		                     inst.branch_target != program.instructions.back().pc);
		const bool local_store =
		    inst.opcode == Opcode::DS_WRITE_B32 || inst.opcode == Opcode::DS_WRITE2_B32;
		const bool buffer_store = inst.opcode == Opcode::BUFFER_STORE_DWORD ||
		                          inst.opcode == Opcode::BUFFER_STORE_DWORDX2 ||
		                          inst.opcode == Opcode::BUFFER_STORE_DWORDX3 ||
		                          inst.opcode == Opcode::BUFFER_STORE_DWORDX4;
		const bool control_store =
		    !local && buffer_store && inst.src2.kind == OperandKind::Sgpr && inst.src2.reg == 2u;
		const bool control_read =
		    !local && (inst.opcode == Opcode::DS_READ_B32 || inst.opcode == Opcode::DS_READ2_B32);
		if ((local && local_store) || control_store || control_read) {
			const auto address = read(inst.src0);
			if (address.kind != Kind::Affine) {
				EXIT("%s tessellation address is not affine at pc 0x%08x (%s)\n",
				     local ? "LS" : "HS", inst.pc, InstructionToString(inst).c_str());
			}
			if (address.coefficient != 0u) {
				const auto expected = control_read ? input_stride : stride;
				EXIT_NOT_IMPLEMENTED((address.coefficient & 3u) != 0u ||
				                     (expected != 0u && expected != address.coefficient));
				if (!control_read) stride = address.coefficient;
			}
		}
		// Store vdata is a source, despite occupying the decoder's dst field.
		if (local_store || buffer_store) continue;
		const auto lhs   = read(inst.src0);
		const auto rhs   = read(inst.src1);
		const auto third = read(inst.src2);
		Address    value;
		switch (inst.opcode) {
			case Opcode::V_MOV_B32:
			case Opcode::S_MOV_B32:
			case Opcode::S_MOVK_I32: value = lhs; break;
			case Opcode::V_READFIRSTLANE_B32:
				if (lhs.kind == Kind::Affine && lhs.coefficient == 0u) value = lhs;
				break;
			case Opcode::V_BFE_U32: value = bit_extract(lhs, rhs, third); break;
			case Opcode::S_BFE_U32:
				if (rhs.kind == Kind::Affine && rhs.coefficient == 0u) {
					value = bit_extract(lhs, constant(rhs.constant & 0x1fu),
					                    constant((rhs.constant >> 16u) & 0x7fu));
				}
				break;
			case Opcode::V_AND_B32:
			case Opcode::S_AND_B32: value = bitwise_and(lhs, rhs); break;
			case Opcode::V_ADD_NC_U32:
			case Opcode::V_ADD_I32:
			case Opcode::S_ADD_U32:
			case Opcode::S_ADD_I32: value = add(lhs, rhs); break;
			case Opcode::V_ADD3_U32: value = add(add(lhs, rhs), third); break;
			case Opcode::V_SUB_NC_U32:
			case Opcode::S_SUB_U32:
			case Opcode::S_SUB_I32: value = subtract(lhs, rhs); break;
			case Opcode::V_SUBREV_NC_U32: value = subtract(rhs, lhs); break;
			case Opcode::V_MUL_U32_U24: value = multiply(low24(lhs), low24(rhs)); break;
			case Opcode::V_MUL_LO_U32:
			case Opcode::V_MUL_LO_I32:
			case Opcode::S_MUL_I32:
			case Opcode::S_MULK_I32: value = multiply(lhs, rhs); break;
			case Opcode::V_MAD_U32_U24: value = add(multiply(low24(lhs), low24(rhs)), third); break;
			case Opcode::V_LSHL_ADD_U32: value = add(shift_left(lhs, rhs), third); break;
			case Opcode::V_ADD_LSHL_U32: value = shift_left(add(lhs, rhs), third); break;
			case Opcode::V_LSHLREV_B32: value = shift_left(rhs, lhs); break;
			case Opcode::V_LSHL_B32:
			case Opcode::S_LSHL_B32: value = shift_left(lhs, rhs); break;
			case Opcode::V_LSHRREV_B32: value = shift_right(rhs, lhs); break;
			case Opcode::V_LSHR_B32:
			case Opcode::S_LSHR_B32: value = shift_right(lhs, rhs); break;
			default: break;
		}
		define(inst.dst, value, inst.data_dwords);
	}
	EXIT_NOT_IMPLEMENTED(stride == 0u);
	return stride;
}

const IR::Inst* TessellationBufferBase(const IR::Inst& inst) {
	if (IR::BufferAccessOf(inst.GetOpcode()) == IR::BufferAccess::None || inst.NumArgs() <= 3u)
		return nullptr;
	const auto* base = inst.Arg(3).Resolve().TryInstruction();
	return base != nullptr && base->GetOpcode() == IR::ValueOpcode::TessellationBase ? base
	                                                                                 : nullptr;
}

IR::Value ActiveAddress(IR::Block& block, IR::Block::iterator before, IR::Value value,
                        IR::Value predicate, std::unordered_map<IR::Inst*, IR::Value>& resolved) {
	using namespace IR;
	value        = value.Resolve();
	auto* source = value.TryInstruction();
	if (source == nullptr) return value;
	if (const auto found = resolved.find(source); found != resolved.end()) return found->second;
	if (source->GetOpcode() == ValueOpcode::SelectU32) {
		return source->Arg(0).Resolve() == predicate.Resolve()
		           ? ActiveAddress(block, before, source->Arg(1), predicate, resolved)
		           : value;
	}
	if (source->GetOpcode() != ValueOpcode::IAdd32 && source->GetOpcode() != ValueOpcode::ISub32)
		return value;
	const auto lhs = ActiveAddress(block, before, source->Arg(0), predicate, resolved);
	const auto rhs = ActiveAddress(block, before, source->Arg(1), predicate, resolved);
	if (lhs != source->Arg(0).Resolve() || rhs != source->Arg(1).Resolve()) {
		auto copy = block.PrependNewInst(before, source->GetOpcode(), {lhs, rhs},
		                                 source->Flags<uint64_t>());
		value     = Value(&*copy);
	}
	resolved.emplace(source, value);
	return value;
}

} // namespace

void AnalyzeTessellationPrograms(std::span<const uint32_t> local, std::span<const uint32_t> control,
                                 ShaderTessellationInputInfo& info) {
	const auto       local_program = Decoder::DecodeFrontProgram(local);
	Decoder::Program control_program;
	Decoder::DecodeProgram(control, control_program);
	info.ls_stride = ReflectTessellationStride(local_program, true, info);
	info.hs_stride = ReflectTessellationStride(control_program, false, info);
	LOGF("Tessellation interface: input_cp=%u output_cp=%u ls_stride=%u hs_stride=%u\n",
	     info.input_control_points, info.output_control_points, info.ls_stride, info.hs_stride);
}

void LowerTessellationMemory(IR::Program& program, const CompileOptions& options) {
	using namespace IR;
	if (options.stage != ShaderType::Local && options.stage != ShaderType::TessellationControl &&
	    options.stage != ShaderType::TessellationEvaluation) {
		return;
	}
	const auto& tess = options.input_info.vertex->tess;
	// Ring addresses can reuse data VGPRs. Their inactive values are irrelevant to
	// a store guarded by the same EXEC predicate, but must remain intact elsewhere.
	for (auto* block: program.blocks) {
		for (auto it = block->begin(); it != block->end(); ++it) {
			if (TessellationBufferBase(*it) == nullptr) continue;
			std::unordered_map<Inst*, Value> resolved;
			it->SetArg(
			    2, ActiveAddress(*block, it, it->Arg(2), it->Arg(it->NumArgs() - 1u), resolved));
		}
	}
	ConstantPropagationPass(program.blocks);
	uint32_t reads = 0, writes = 0, factors = 0;
	for (auto* block: program.blocks) {
		for (auto it = block->begin(); it != block->end(); ++it) {
			auto&      inst   = *it;
			const auto shared = SharedAccessOf(inst.GetOpcode());
			const auto buffer = BufferAccessOf(inst.GetOpcode());
			if (shared == SharedAccess::None && buffer == BufferAccess::None) {
				continue;
			}
			const auto&           memory = program.memory_info.at(inst.Flags<MemoryFlags>().index);
			bool                  write  = false;
			uint32_t              components = 0;
			TessellationAttribute kind;
			Value                 address, predicate;
			if (shared != SharedAccess::None && memory.kind == ResourceKind::Lds) {
				write = shared == SharedAccess::Write;
				EXIT_NOT_IMPLEMENTED(memory.data_bits != 32u ||
				                     (options.stage == ShaderType::Local
				                          ? !write
				                          : options.stage != ShaderType::TessellationControl ||
				                                shared != SharedAccess::Read));
				kind       = write ? TessellationAttribute::LocalOutput
				                   : TessellationAttribute::ControlInput;
				components = SharedComponentCount(inst.GetOpcode());
				address    = inst.Arg(0);
				predicate  = inst.Arg(inst.NumArgs() - 1u);
			} else if (buffer != BufferAccess::None && memory.kind == ResourceKind::Buffer) {
				const auto* base = TessellationBufferBase(inst);
				if (base == nullptr) {
					continue;
				}
				EXIT_NOT_IMPLEMENTED(memory.data_bits != 32u || memory.formatted || memory.idxen ||
				                     !memory.offen || buffer == BufferAccess::Atomic);
				write      = buffer == BufferAccess::Write;
				components = BufferComponentCount(inst.GetOpcode());
				address    = inst.Arg(2).Resolve();
				predicate  = inst.Arg(inst.NumArgs() - 1u);
				if (base->Arg(0).U32() == 1u) {
					EXIT_NOT_IMPLEMENTED(!write ||
					                     options.stage != ShaderType::TessellationControl);
					kind = TessellationAttribute::Factor;
					factors += components;
				} else {
					kind = write ? TessellationAttribute::ControlOutput
					             : TessellationAttribute::EvaluationInput;
					EXIT_NOT_IMPLEMENTED(write
					                         ? options.stage != ShaderType::TessellationControl
					                         : options.stage != ShaderType::TessellationEvaluation);
					if (address.IsImmediate() &&
					    address.U32() >= tess.hs_stride * tess.output_control_points) {
						EXIT_NOT_IMPLEMENTED(!write);
						kind = TessellationAttribute::PatchOutput;
					}
				}
			} else {
				continue;
			}
			const auto emit = [&](ValueOpcode opcode, std::initializer_list<Value> args) {
				return Value(&*block->PrependNewInst(it, opcode, args));
			};
			std::array<Value, 4> values;
			for (uint32_t component = 0; component < components; component++) {
				const auto offset = memory.offset + 4u * component;
				const auto byte_address =
				    offset == 0u ? address : emit(ValueOpcode::IAdd32, {address, Value(offset)});
				if (write) {
					Value data = shared != SharedAccess::None ? inst.Arg(component + 1u)
					             : components == 1u
					                 ? inst.Arg(4)
					                 : emit(components == 2u   ? ValueOpcode::CompositeExtractU32x2
					                        : components == 3u ? ValueOpcode::CompositeExtractU32x3
					                                           : ValueOpcode::CompositeExtractU32x4,
					                        {inst.Arg(4), Value(component)});
					emit(ValueOpcode::SetTessellationAttribute,
					     {Value(static_cast<uint32_t>(kind)), byte_address, data, predicate});
					writes++;
				} else {
					values[component] =
					    emit(ValueOpcode::GetTessellationAttribute,
					         {Value(static_cast<uint32_t>(kind)), byte_address, predicate});
					reads++;
				}
			}
			if (!write) {
				Value replacement = values[0];
				if (components == 2u)
					replacement =
					    emit(ValueOpcode::CompositeConstructU32x2, {values[0], values[1]});
				if (components == 3u)
					replacement = emit(ValueOpcode::CompositeConstructU32x3,
					                   {values[0], values[1], values[2]});
				if (components == 4u)
					replacement = emit(ValueOpcode::CompositeConstructU32x4,
					                   {values[0], values[1], values[2], values[3]});
				inst.ReplaceUsesWith(replacement);
			}
			inst.Invalidate();
		}
	}
	ConstantPropagationPass(program.blocks);
	RemoveIdentities(program.blocks);
	EliminateDeadCode(program.blocks);
	LOGF("%s tessellation lowering: reads=%u writes=%u factors=%u\n",
	     options.stage == ShaderType::Local                 ? "LS"
	     : options.stage == ShaderType::TessellationControl ? "HS"
	                                                       : "TES",
	     reads, writes, factors);
}

} // namespace Libs::Graphics::ShaderRecompiler
