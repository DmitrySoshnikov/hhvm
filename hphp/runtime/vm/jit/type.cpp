/*
   +----------------------------------------------------------------------+
   | HipHop for PHP                                                       |
   +----------------------------------------------------------------------+
   | Copyright (c) 2010-2014 Facebook, Inc. (http://www.facebook.com)     |
   +----------------------------------------------------------------------+
   | This source file is subject to version 3.01 of the PHP license,      |
   | that is bundled with this package in the file LICENSE, and is        |
   | available through the world-wide-web at the following url:           |
   | http://www.php.net/license/3_01.txt                                  |
   | If you did not receive a copy of the PHP license and are unable to   |
   | obtain it through the world-wide-web, please send a note to          |
   | license@php.net so we can mail you a copy immediately.               |
   +----------------------------------------------------------------------+
*/

#include "hphp/runtime/vm/jit/type.h"

#include <boost/algorithm/string/trim.hpp>

#include "folly/Conv.h"
#include "folly/Format.h"
#include "folly/MapUtil.h"
#include "folly/gen/Base.h"

#include "hphp/util/abi-cxx.h"
#include "hphp/util/text-util.h"
#include "hphp/util/trace.h"
#include "hphp/runtime/base/repo-auth-type-array.h"
#include "hphp/runtime/vm/jit/ir-opcode.h"
#include "hphp/runtime/vm/jit/ir-instruction.h"
#include "hphp/runtime/vm/jit/print.h"
#include "hphp/runtime/vm/jit/ssa-tmp.h"
#include "hphp/runtime/vm/jit/translator.h"

#include <vector>

namespace HPHP { namespace jit {

TRACE_SET_MOD(hhir);

//////////////////////////////////////////////////////////////////////

#define IRT(name, ...) const Type Type::name(Type::k##name);
IR_TYPES
#undef IRT

std::string Type::constValString() const {
  assert(isConst());

  if (subtypeOf(Int)) {
    return folly::format("{}", m_intVal).str();
  } else if (subtypeOf(Dbl)) {
    // don't format doubles as integers.
    auto s = folly::format("{}", m_dblVal).str();
    if (!strchr(s.c_str(), '.') && !strchr(s.c_str(), 'e')) {
      return folly::format("{:.1f}", m_dblVal).str();
    }
    return s;
  } else if (subtypeOf(Bool)) {
    return m_boolVal ? "true" : "false";
  } else if (subtypeOf(StaticStr)) {
    auto str = m_strVal;
    return folly::format("\"{}\"", escapeStringForCPP(str->data(),
                                                      str->size())).str();
  } else if (subtypeOf(StaticArr)) {
    if (m_arrVal->empty()) {
      return "array()";
    }
    return folly::format("Array({})", m_arrVal).str();
  } else if (subtypeOf(Func)) {
    return folly::format("Func({})", m_funcVal ? m_funcVal->fullName()->data()
                                               : "nullptr").str();
  } else if (subtypeOf(Cls)) {
    return folly::format("Cls({})", m_clsVal ? m_clsVal->name()->data()
                                             : "nullptr").str();
  } else if (subtypeOf(Cctx)) {
    if (!m_intVal) {
      return "Cctx(Cls(nullptr))";
    }
    const Class* cls = m_cctxVal.cls();
    return folly::format("Cctx(Cls({}))", cls->name()->data()).str();
  } else if (subtypeOf(TCA)) {
    auto name = getNativeFunctionName(m_tcaVal);
    const char* hphp = "HPHP::";

    if (!name.compare(0, strlen(hphp), hphp)) {
      name = name.substr(strlen(hphp));
    }
    auto pos = name.find_first_of('(');
    if (pos != std::string::npos) {
      name = name.substr(0, pos);
    }
    return folly::format("TCA: {}({})", m_tcaVal, boost::trim_copy(name)).str();
  } else if (subtypeOf(RDSHandle)) {
    return folly::format("RDS::Handle({:#x})", m_rdsHandleVal).str();
  } else if (subtypeOfAny(Null, Nullptr) || isPtr()) {
    return toString();
  } else {
    not_reached();
  }
}

std::string Type::toString() const {
  // Try to find an exact match to a predefined type
# define IRT(name, ...) if (*this == name) return #name;
  IR_TYPES
# undef IRT

  if (isBoxed()) {
    return folly::to<std::string>("Boxed", innerType().toString());
  }
  if (isPtr()) {
    auto ret = folly::to<std::string>("PtrTo", deref().toString());
    if (isConst()) ret += folly::format("({})", m_ptrVal).str();
    return ret;
  }

  if (m_hasConstVal) {
    return folly::format("{}<{}>",
                         dropConstVal().toString(), constValString()).str();
  }

  auto t = *this;
  std::vector<std::string> parts;
  if (isSpecialized()) {
    if (canSpecializeClass()) {
      assert(getClass());

      auto const base = Type(m_bits & kAnyObj).toString();
      auto const exact = getExactClass() ? "=" : "<=";
      auto const name = getClass()->name()->data();
      auto const partStr = folly::to<std::string>(base, exact, name);

      parts.push_back(partStr);
      t -= AnyObj;
    } else if (canSpecializeArray()) {
      auto str = folly::to<std::string>(Type(m_bits & kAnyArr).toString());
      if (hasArrayKind()) {
        str += "=";
        str += ArrayData::kindToString(getArrayKind());
      }
      if (auto ty = getArrayType()) {
        str += folly::to<std::string>(':', show(*ty));
      }
      parts.push_back(str);
      t -= AnyArr;
    } else {
      not_reached();
    }
  }

  // Concat all of the primitive types in the custom union type
# define IRT(name, ...) if (name <= t) parts.push_back(#name);
  IRT_PRIMITIVE
# undef IRT
  assert(!parts.empty());
  if (parts.size() == 1) {
    return parts.front();
  }
  return folly::format("{{{}}}", folly::join('|', parts)).str();
}

std::string Type::debugString(Type t) {
  return t.toString();
}

bool Type::checkValid() const {
  if (m_extra) {
    assert((!(m_bits & kAnyObj) || !(m_bits & kAnyArr)) &&
           "Conflicting specialization");
  }

  return true;
}

Type Type::unionOf(Type t1, Type t2) {
  if (t1 == t2 || t2 < t1) return t1;
  if (t1 < t2) return t2;
  static const Type union_types[] = {
#   define IRT(name, ...) name,
    IRT_PHP(IRT_BOXES)
    IRT_PHP_UNIONS(IRT_BOXES)
#   undef IRT
    Gen,
    PtrToGen,
  };
  Type t12 = t1 | t2;
  for (auto u : union_types) {
    if (t12 <= u) return u;
  }
  not_reached();
}

DataType Type::toDataType() const {
  assert(!isPtr());
  assert(isKnownDataType());

  // Order is important here: types must progress from more specific
  // to less specific to return the most specific DataType.
  if (subtypeOf(Uninit))        return KindOfUninit;
  if (subtypeOf(InitNull))      return KindOfNull;
  if (subtypeOf(Bool))          return KindOfBoolean;
  if (subtypeOf(Int))           return KindOfInt64;
  if (subtypeOf(Dbl))           return KindOfDouble;
  if (subtypeOf(StaticStr))     return KindOfStaticString;
  if (subtypeOf(Str))           return KindOfString;
  if (subtypeOf(Arr))           return KindOfArray;
  if (subtypeOf(Obj))           return KindOfObject;
  if (subtypeOf(Res))           return KindOfResource;
  if (subtypeOf(BoxedCell))     return KindOfRef;
  if (subtypeOf(Cls))           return KindOfClass;
  always_assert_flog(false,
                     "Bad Type {} in Type::toDataType()", *this);
}

Type::Type(const DynLocation* dl)
  : Type(dl->rtt)
{}

Type::bits_t Type::bitsFromDataType(DataType outer, DataType inner) {
  assert(outer != KindOfInvalid);
  assert(inner != KindOfRef);
  assert(IMPLIES(inner == KindOfNone, outer != KindOfRef));

  switch (outer) {
    case KindOfUninit        : return kUninit;
    case KindOfNull          : return kInitNull;
    case KindOfBoolean       : return kBool;
    case KindOfInt64         : return kInt;
    case KindOfDouble        : return kDbl;
    case KindOfStaticString  : return kStaticStr;
    case KindOfString        : return kStr;
    case KindOfArray         : return kArr;
    case KindOfResource      : return kRes;
    case KindOfObject        : return kObj;
    case KindOfClass         : return kCls;
    case KindOfAny           : return kGen;
    case KindOfRef: {
      if (inner == KindOfAny) {
        return kBoxedCell;
      } else {
        assert(inner != KindOfUninit);
        return bitsFromDataType(inner, KindOfNone) << kBoxShift;
      }
    }
    default                  : always_assert(false && "Unsupported DataType");
  }
}

// ClassOps and ArrayOps are used below to write code that can perform set
// operations on both Class and ArrayKind specializations.
struct Type::ClassOps {
  static bool subtypeOf(ClassInfo a, ClassInfo b) {
    return a == b || (a.get()->classof(b.get()) && !b.isExact());
  }

  static folly::Optional<ClassInfo> commonAncestor(ClassInfo a,
                                                   ClassInfo b) {
    if (!isNormalClass(a.get()) || !isNormalClass(b.get())) return folly::none;
    if (auto result = a.get()->commonAncestor(b.get())) {
      return ClassInfo(result, ClassTag::Sub);
    }

    return folly::none;
  }

  static folly::Optional<ClassInfo> intersect(ClassInfo a, ClassInfo b) {
    // There shouldn't be any cases we could cover here that aren't already
    // handled by the subtype checks.
    return folly::none;
  }
};

struct Type::ArrayOps {
  static bool subtypeOf(ArrayInfo a, ArrayInfo b) {
    if (a == b) return true;
    if (!arrayType(b) && !arrayKindValid(b)) return true;
    return false;
  }

  static folly::Optional<ArrayInfo> commonAncestor(ArrayInfo a, ArrayInfo b) {
    if (a == b) return a;
    auto const sameKind = [&]() -> folly::Optional<ArrayData::ArrayKind> {
      if (arrayKindValid(a)) {
        if (arrayKindValid(b)) {
          if (a == b) return arrayKind(a);
          return folly::none;
        }
        return arrayKind(a);
      }
      if (arrayKindValid(b)) return arrayKind(b);
      return folly::none;
    }();
    auto const ty = [&]() -> const RepoAuthType::Array* {
      auto ata = arrayType(a);
      auto atb = arrayType(b);
      return ata && atb ? (ata == atb ? ata : nullptr) :
             ata ? ata : atb;
    }();
    if (ty || sameKind) return makeArrayInfo(sameKind, ty);
    return folly::none;
  }

  static folly::Optional<ArrayInfo> intersect(ArrayInfo a, ArrayInfo b) {
    assert(a != b);

    auto const aka = okind(a);
    auto const akb = okind(b);
    auto const ata = arrayType(a);
    auto const atb = arrayType(b);
    if (aka == akb) {
      // arrayType must be non-equal by above assertion.  Since the
      // kinds are the same, as long as one is null we can keep the
      // other.
      assert(ata != atb);
      if (ata && atb) return makeArrayInfo(aka, nullptr);
      return makeArrayInfo(aka, ata ? ata : atb);
    }
    if (aka && akb) {
      assert(aka != akb);
      if (ata == atb) {
        return makeArrayInfo(folly::none, ata);
      }
      return folly::none;
    }
    assert(aka.hasValue() || akb.hasValue());
    assert(!(aka.hasValue() && akb.hasValue()));
    if (akb && !aka) return intersect(b, a);
    assert(aka.hasValue() && !akb.hasValue());

    if (!atb) return makeArrayInfo(aka, ata /* could be null */);
    if (!ata) return makeArrayInfo(aka, atb /* could be null */);
    return makeArrayInfo(aka, ata == atb ? ata : nullptr);
  }

private:
  static folly::Optional<ArrayData::ArrayKind> okind(ArrayInfo in) {
    if (arrayKindValid(in)) return arrayKind(in);
    return folly::none;
  }
};

// Union and Intersect implement part of the logic for operator| and operator&,
// respectively. Each has two static methods:
//
// combineSame: called when at least one of *this or b is specialized and
//              they can both specialize on the same type.
// combineDifferent: called when *this and b can specialize different ways
//                   and at least one of the two is specialized.

struct Type::Union {
  template<typename Ops, typename T>
  static Type combineSame(bits_t bits, bits_t typeMask,
                          folly::Optional<T> aOpt,
                          folly::Optional<T> bOpt) {
    // If one or both types are not specialized, the specialization is lost
    if (!(aOpt && bOpt)) return Type(bits);

    auto const a = *aOpt;
    auto const b = *bOpt;

    // If the specialization is the same, keep it.
    if (a == b)            return Type(bits, a);

    // If one is a subtype of the other, their union is the least specific of
    // the two.
    if (Ops::subtypeOf(a, b))     return Type(bits, b);
    if (Ops::subtypeOf(b, a))     return Type(bits, a);

    // Check for a common ancestor.
    if (auto p = Ops::commonAncestor(a, b)) return Type(bits, *p);

    // a and b are unrelated but we can't hold both of them in a Type. Dropping
    // the specialization returns a supertype of their true union. It's not
    // optimal but not incorrect.
    return Type(bits);
  }

  static Type combineDifferent(bits_t newBits, Type a, Type b) {
    // a and b can specialize differently, so their union can't have any
    // specialization (it would be an ambiguously specialized type).
    return Type(newBits);
  }
};

struct Type::Intersect {
  template<typename Ops, typename T>
  static Type combineSame(bits_t bits, bits_t typeMask,
                          folly::Optional<T> aOpt,
                          folly::Optional<T> bOpt) {
    if (!bits) return Type::Bottom;

    // We shouldn't get here if neither is specialized.
    assert(aOpt || bOpt);

    // If we know both, attempt to combine them.
    if (aOpt && bOpt) {
      auto const a = *aOpt;
      auto const b = *bOpt;

      // When a and b are the same, keep the specialization.
      if (a == b)        return Type(bits, a);

      // If one is a subtype of the other, their intersection is the most
      // specific of the two.
      if (Ops::subtypeOf(a, b)) return Type(bits, a);
      if (Ops::subtypeOf(b, a)) return Type(bits, b);

      // If we can intersect the specializations, use that.
      if (auto info = Ops::intersect(a, b)) return Type(bits, *info);

      // a and b are unrelated so we have to remove the specialized type. This
      // means dropping the specialization and the bits that correspond to the
      // type that was specialized.
      return Type(bits & ~typeMask);
    }

    if (aOpt) return Type(bits, *aOpt);
    if (bOpt) return Type(bits, *bOpt);

    not_reached();
  }

  static Type combineDifferent(bits_t newBits, Type a, Type b) {
    // Since a and b are each eligible for different specializations, their
    // intersection can't have any specialization left.
    return Type(newBits);
  }
};

/*
 * combine handles the cases that have similar shapes between & and |: neither
 * is specialized or both have the same possible specialization type. Other
 * cases delegate back to Oper.
 */
template<typename Oper>
Type Type::combine(bits_t newBits, Type a, Type b) {
  static_assert(std::is_same<Oper, Union>::value ||
                std::is_same<Oper, Intersect>::value,
                "Type::combine given unsupported template argument");

  // If neither type is specialized, the result is simple.
  if (LIKELY(!a.isSpecialized() && !b.isSpecialized())) {
    return Type(newBits);
  }

  // If one of the types can't be specialized while the other is specialized,
  // preserve the specialization.
  if (!a.canSpecializeAny() || !b.canSpecializeAny()) {
    auto const specType = a.isSpecialized() ? a.specializedType()
                                            : b.specializedType();

    // If the specialized type doesn't exist in newBits, drop the
    // specialization.
    if (newBits & specType.m_bits) return Type(newBits, specType.m_extra);
    return Type(newBits);
  }

  // If both types are eligible for the same kind of specialization and at
  // least one is specialized, delegate to Oper::combineSame.
  if (a.canSpecializeClass() && b.canSpecializeClass()) {
    folly::Optional<ClassInfo> aClass, bClass;
    if (a.getClass()) aClass = a.m_class;
    if (b.getClass()) bClass = b.m_class;

    return Oper::template combineSame<ClassOps>(newBits, kAnyObj,
                                                aClass, bClass);
  }

  if (a.canSpecializeArray() && b.canSpecializeArray()) {
    folly::Optional<ArrayInfo> aInfo, bInfo;
    if (a.hasArrayKind() || a.getArrayType()) {
      aInfo = a.m_arrayInfo;
    }
    if (b.hasArrayKind() || b.getArrayType()) {
      bInfo = b.m_arrayInfo;
    }

    return Oper::template combineSame<ArrayOps>(newBits, kAnyArr, aInfo,
      bInfo);
  }

  // The types are eligible for different kinds of specialization and at least
  // one is specialized, so delegate to Oper::combineDifferent.
  return Oper::combineDifferent(newBits, a, b);
}

Type Type::operator|(Type b) const {
  auto a = *this;

  // Representing types like {Int<12>|Arr} could get messy and isn't useful in
  // practice, so unless we're unioning a constant type with itself or Bottom,
  // drop the constant value(s).
  if (a == b || b == Bottom) return a;
  if (a == Bottom) return b;

  a = a.dropConstVal();
  b = b.dropConstVal();

  return combine<Union>(a.m_bits | b.m_bits, a, b);
}

Type Type::operator&(Type b) const {
  auto a = *this;
  auto const newBits = a.m_bits & b.m_bits;

  // When intersecting a constant value with another type, the result will be
  // the constant value if the other value is a supertype of the constant, and
  // Bottom otherwise.
  if (a.m_hasConstVal) return a <= b ? a : Bottom;
  if (b.m_hasConstVal) return b <= a ? b : Bottom;

  return combine<Intersect>(newBits, a, b);
}

Type Type::operator-(Type other) const {
  auto const newBits = m_bits & ~other.m_bits;

  if (m_hasConstVal) {
    // If other is a constant of the same type, the result is Bottom or this
    // depending on whether or not it's the same constant.
    if (other.m_bits == m_bits && other.m_hasConstVal) {
      return other.m_extra == m_extra ? Bottom : *this;
    }

    // Otherwise, just check to see if the constant's type was removed in
    // newBits.
    return (newBits & m_bits) ? *this : Bottom;
  }

  // Rather than try to represent types like "all Ints except 24", treat t -
  // Int<24> as t - Int.
  other = other.dropConstVal();

  auto const spec1 = isSpecialized();
  auto const spec2 = other.isSpecialized();

  // The common easy case is when neither type is specialized.
  if (LIKELY(!spec1 && !spec2)) return Type(newBits);

  if (spec1 && spec2) {
    if (canSpecializeClass() != other.canSpecializeClass()) {
      // Both are specialized but in different ways. Our specialization is
      // preserved.
      return Type(newBits, m_extra);
    }

    // Subtracting different specializations of the same type could get messy
    // so we don't support it for now.
    always_assert(specializedType() == other.specializedType() &&
                  "Incompatible specialized types given to operator-");

    // If we got here, both types have the same specialization, so it's removed
    // from the result.
    return Type(newBits);
  }

  // If masking out other's bits removed all of the bits that correspond to our
  // specialization, take it out. Otherwise, preserve it.
  if (spec1) {
    if (canSpecializeClass()) {
      if (!(newBits & kAnyObj)) return Type(newBits);
      return Type(newBits, m_class);
    }
    if (canSpecializeArray()) {
      if (!(newBits & kAnyArr)) return Type(newBits);
      return Type(newBits, m_arrayInfo);
    }
    not_reached();
  }

  // Only other is specialized. This is where things get a little fuzzy. We
  // want to be able to support things like Obj - Obj<C> but we can't represent
  // Obj<~C>. We compromise and return Bottom in cases like this, which means
  // we need to be careful because (a - b) == Bottom doesn't imply a <= b in
  // this world.
  if (other.canSpecializeClass()) return Type(newBits & ~kAnyObj);
  return Type(newBits & ~kAnyArr);
}

bool Type::subtypeOfSpecialized(Type t2) const {
  assert((m_bits & t2.m_bits) == m_bits);
  assert(!t2.m_hasConstVal);
  assert(t2.isSpecialized());

  // Since t2 is specialized, we must either not be eligible for the same kind
  // of specialization (Int <= {Int|Arr<Packed>}) or have a specialization
  // that is a subtype of t2's specialization.
  if (t2.canSpecializeClass()) {
    if (!isSpecialized()) return false;

    //  Obj=A <:  Obj=A
    // Obj<=A <: Obj<=A
    if (m_class.isExact() == t2.m_class.isExact() &&
        getClass() == t2.getClass()) {
      return true;
    }

    //      A <: B
    // ----------------
    //  Obj=A <: Obj<=B
    // Obj<=A <: Obj<=B
    if (!t2.m_class.isExact()) return getClass()->classof(t2.getClass());
    return false;
  }

  assert(t2.canSpecializeArray());
  if (!canSpecializeArray()) return true;
  if (!isSpecialized()) return false;

  // Both types are specialized Arr types. "Specialized" in this context
  // means it has at least one of a RepoAuthType::Array* or (const ArrayData*
  // or ArrayData::ArrayKind). We may return false erroneously in cases where
  // a 100% accurate comparison of the specializations would be prohibitively
  // expensive.
  if (m_arrayInfo == t2.m_arrayInfo) return true;
  auto rat1 = getArrayType();
  auto rat2 = t2.getArrayType();

  if (rat1 != rat2 && !(rat1 && !rat2)) {
    // Different rats are only ok if rat1 is present and rat2 isn't. It's
    // possible for one rat to be a subtype of another rat or array kind, but
    // checking that can be very expensive.
    return false;
  }

  auto kind1 = getOptArrayKind();
  auto kind2 = t2.getOptArrayKind();
  assert(kind1 || kind2);
  if (kind1 && !kind2) return true;
  if (kind2 && !kind1) return false;
  if (*kind1 != *kind2) return false;

  // Same kinds but we still have to check for const arrays. a <= b iff they
  // have the same const array or a has a const array and b doesn't. If they
  // have the same non-nullptr const array the m_arrayInfo check up above
  // should've triggered.
  auto const1 = isConst() ? arrVal() : nullptr;
  auto const2 = t2.isConst() ? t2.arrVal() : nullptr;
  assert((!const1 && !const2) || const1 != const2);
  return const1 == const2 || (const1 && !const2);
}

Type liveTVType(const TypedValue* tv) {
  assert(tv->m_type == KindOfClass || tvIsPlausible(*tv));

  if (tv->m_type == KindOfObject) {
    Class* cls = tv->m_data.pobj->getVMClass();

    // We only allow specialization on classes that can't be
    // overridden for now. If this changes, then this will need to
    // specialize on sub object types instead.
    if (!cls || !(cls->attrs() & AttrNoOverride)) return Type::Obj;
    return Type::Obj.specializeExact(cls);
  }
  if (tv->m_type == KindOfArray) {
    return Type::Arr.specialize(tv->m_data.parr->kind());
  }

  auto outer = tv->m_type;
  auto inner = KindOfInvalid;

  if (outer == KindOfStaticString) outer = KindOfString;
  if (outer == KindOfRef) {
    inner = tv->m_data.pref->tv()->m_type;
    if (inner == KindOfStaticString) inner = KindOfString;
  }
  return Type(outer, inner);
}

Type Type::relaxToGuardable() const {
  auto const ty = unspecialize();

  if (ty.isKnownDataType()) return ty;

  if (ty.subtypeOf(UncountedInit)) return Type::UncountedInit;
  if (ty.subtypeOf(Uncounted)) return Type::Uncounted;
  if (ty.subtypeOf(Cell)) return Type::Cell;
  if (ty.subtypeOf(BoxedCell)) return Type::BoxedCell;
  if (ty.subtypeOf(Gen)) return Type::Gen;
  not_reached();
}

//////////////////////////////////////////////////////////////////////

namespace {

Type setElemReturn(const IRInstruction* inst) {
  assert(inst->op() == SetElem || inst->op() == SetElemStk);
  auto baseType = inst->src(minstrBaseIdx(inst))->type().strip();

  // If the base is a Str, the result will always be a CountedStr (or
  // an exception). If the base might be a str, the result wil be
  // CountedStr or Nullptr. Otherwise, the result is always Nullptr.
  if (baseType.subtypeOf(Type::Str)) {
    return Type::CountedStr;
  } else if (baseType.maybe(Type::Str)) {
    return Type::CountedStr | Type::Nullptr;
  }
  return Type::Nullptr;
}

Type builtinReturn(const IRInstruction* inst) {
  assert(inst->op() == CallBuiltin);

  Type t = inst->typeParam();
  if (t.isSimpleType() || t.equals(Type::Cell)) {
    return t;
  }
  if (t.isReferenceType() || t.equals(Type::BoxedCell)) {
    return t | Type::InitNull;
  }
  not_reached();
}

Type stkReturn(const IRInstruction* inst, int dstId,
               std::function<Type()> inner) {
  assert(inst->modifiesStack());
  if (dstId == 0 && inst->hasMainDst()) {
    // Return the type of the main dest (if one exists) as dst 0
    return inner();
  }
  // The instruction modifies the stack and this isn't the main dest,
  // so it's a StkPtr.
  return Type::StkPtr;
}

Type thisReturn(const IRInstruction* inst) {
  auto fpInst = inst->src(0)->inst();

  // Find the instruction that created the current frame and grab the context
  // class from it. $this, if present, is always going to be the context class
  // or a subclass of the context.
  always_assert(fpInst->is(DefFP, DefInlineFP));
  auto const func = fpInst->is(DefFP) ? fpInst->marker().func()
                                      : fpInst->extra<DefInlineFP>()->target;
  func->validate();
  assert(func->isMethod() || func->isPseudoMain());

  // If the function is a cloned closure which may have a re-bound $this which
  // is not a subclass of the context return an unspecialized type.
  if (func->hasForeignThis()) return Type::Obj;

  return Type::Obj.specialize(func->cls());
}

Type allocObjReturn(const IRInstruction* inst) {
  switch (inst->op()) {
    case ConstructInstance:
      return Type::Obj.specialize(inst->extra<ConstructInstance>()->cls);

    case NewInstanceRaw:
      return Type::Obj.specializeExact(inst->extra<NewInstanceRaw>()->cls);

    case AllocObj:
      return inst->src(0)->isConst()
        ? Type::Obj.specializeExact(inst->src(0)->clsVal())
        : Type::Obj;

    default:
      always_assert(false && "Invalid opcode returning AllocObj");
  }
}

Type arrElemReturn(const IRInstruction* inst) {
  if (inst->op() != LdPackedArrayElem) return Type::Gen;
  auto const arrTy = inst->src(0)->type().getArrayType();
  if (!arrTy) return Type::Gen;

  using T = RepoAuthType::Array::Tag;
  switch (arrTy->tag()) {
  case T::Packed:
    {
      auto const idx = inst->src(1);
      if (!idx->isConst()) return Type::Gen;
      if (idx->intVal() >= 0 && idx->intVal() < arrTy->size()) {
        return convertToType(arrTy->packedElem(idx->intVal()));
      }
    }
    return Type::Gen;
  case T::PackedN:
    return convertToType(arrTy->elemType());
  }

  return Type::Gen;
}

}

Type ldRefReturn(Type typeParam) {
  assert(typeParam.notBoxed());
  // Guarding on specialized types and uncommon unions like {Int|Bool} is
  // expensive enough that we only want to do it in situations where we've
  // manually confirmed the benefit.

  if (typeParam.strictSubtypeOf(Type::Obj) &&
      typeParam.getClass()->attrs() & AttrFinal &&
      typeParam.getClass()->isCollectionClass()) {
    /*
     * This case is needed for the minstr-translator.
     * see MInstrTranslator::checkMIState().
     */
    return typeParam;
  }

  auto const type = typeParam.unspecialize();

  if (type.isKnownDataType())      return type;
  if (type <= Type::UncountedInit) return Type::UncountedInit;
  if (type <= Type::Uncounted)     return Type::Uncounted;
  always_assert(type <= Type::Cell);
  return Type::InitCell;
}

Type boxType(Type t) {
  // If t contains Uninit, replace it with InitNull.
  t = t.maybe(Type::Uninit) ? (t - Type::Uninit) | Type::InitNull : t;
  // We don't try to track when a BoxedStaticStr might be converted to
  // a BoxedStr, and we never guard on staticness for strings, so
  // boxing a string needs to forget this detail.  Same thing for
  // arrays.
  if (t.subtypeOf(Type::Str)) {
    t = Type::Str;
  } else if (t.subtypeOf(Type::Arr)) {
    t = Type::Arr;
  }
  // When boxing an Object, if the inner class does not have AttrNoOverride,
  // drop the class specialization.
  if (t < Type::Obj && !(t.getClass()->attrs() & AttrNoOverride)) {
    t = t.unspecialize();
  }
  // Everything else is just a pure type-system boxing operation.
  return t.box();
}

Type convertToType(RepoAuthType ty) {
  using T = RepoAuthType::Tag;
  switch (ty.tag()) {
  case T::OptBool:        return Type::Bool      | Type::InitNull;
  case T::OptInt:         return Type::Int       | Type::InitNull;
  case T::OptSStr:        return Type::StaticStr | Type::InitNull;
  case T::OptStr:         return Type::Str       | Type::InitNull;
  case T::OptDbl:         return Type::Dbl       | Type::InitNull;
  case T::OptRes:         return Type::Res       | Type::InitNull;
  case T::OptObj:         return Type::Obj       | Type::InitNull;

  case T::Uninit:         return Type::Uninit;
  case T::InitNull:       return Type::InitNull;
  case T::Null:           return Type::Null;
  case T::Bool:           return Type::Bool;
  case T::Int:            return Type::Int;
  case T::Dbl:            return Type::Dbl;
  case T::Res:            return Type::Res;
  case T::SStr:           return Type::StaticStr;
  case T::Str:            return Type::Str;
  case T::Obj:            return Type::Obj;

  case T::Cell:           return Type::Cell;
  case T::Ref:            return Type::BoxedCell;
  case T::InitUnc:        return Type::UncountedInit;
  case T::Unc:            return Type::Uncounted;
  case T::InitCell:       return Type::InitCell;
  case T::InitGen:        return Type::Init;
  case T::Gen:            return Type::Gen;

  // TODO(#4205897): option specialized array types
  case T::OptArr:         return Type::Arr       | Type::InitNull;
  case T::OptSArr:        return Type::StaticArr | Type::InitNull;

  case T::SArr:
    if (auto const ar = ty.array()) return Type::StaticArr.specialize(ar);
    return Type::StaticArr;
  case T::Arr:
    if (auto const ar = ty.array()) return Type::Arr.specialize(ar);
    return Type::Arr;

  case T::SubObj:
  case T::ExactObj: {
    auto const base = Type::Obj;
    if (auto const cls = Unit::lookupUniqueClass(ty.clsName())) {
      return ty.tag() == T::ExactObj ?
        base.specializeExact(cls) :
        base.specialize(cls);
    }
    return base;
  }
  case T::OptSubObj:
  case T::OptExactObj: {
    auto const base = Type::Obj | Type::InitNull;
    if (auto const cls = Unit::lookupUniqueClass(ty.clsName())) {
      return ty.tag() == T::OptExactObj ?
        base.specializeExact(cls) :
        base.specialize(cls);
    }
    return base;
  }
  }
  not_reached();
}

Type refineTypeNoCheck(Type oldType, Type newType) {
  // It's OK for the old and new inner types of boxed values not to
  // intersect, since the inner type is really just a prediction.
  // But if they do intersect, we keep the intersection.  This is
  // necessary to keep the type known in situations like:
  //   oldType: Boxed{Obj}
  //   newType: Boxed{Obj<C>, InitNull}
  if (oldType.isBoxed() && newType.isBoxed() && oldType.not(newType)) {
    return oldType < newType ? oldType : newType;
  }
  return oldType & newType;
}

Type refineType(Type oldType, Type newType) {
  Type result = refineTypeNoCheck(oldType, newType);
  always_assert_flog(result != Type::Bottom,
                     "refineType({}, {}) failed", oldType, newType);
  return result;
}

namespace TypeNames {
#define IRT(name, ...) UNUSED const Type name = Type::name;
  IR_TYPES
#undef IRT
};

Type outputType(const IRInstruction* inst, int dstId) {
  using namespace TypeNames;
  using TypeNames::TCA;
#define D(type)         return type;
#define DofS(n)         return inst->src(n)->type();
#define DUnbox(n)       return inst->src(n)->type().unbox();
#define DBox(n)         return boxType(inst->src(n)->type());
#define DRefineS(n)     return refineTypeNoCheck(inst->src(n)->type(), \
                                                 inst->typeParam());
#define DParam          return inst->typeParam();
#define DAllocObj       return allocObjReturn(inst);
#define DArrElem        return arrElemReturn(inst);
#define DArrPacked      return Type::Arr.specialize(ArrayData::kPackedKind);
#define DLdRef          return ldRefReturn(inst->typeParam());
#define DThis           return thisReturn(inst);
#define DMulti          return Type::Bottom;
#define DStk(in)        return stkReturn(inst, dstId, \
                                   [&]() -> Type { in not_reached(); });
#define DSetElem        return setElemReturn(inst);
#define ND              assert(0 && "outputType requires HasDest or NaryDest");
#define DBuiltin        return builtinReturn(inst);
#define DSubtract(n, t) return inst->src(n)->type() - t;
#define DLdRaw          return inst->extra<RawMemData>()->info().type;
#define DCns            return Type::Uninit | Type::InitNull | Type::Bool | \
                               Type::Int | Type::Dbl | Type::Str | Type::Res;

#define O(name, dstinfo, srcinfo, flags) case name: dstinfo not_reached();

  switch (inst->op()) {
  IR_OPCODES
  default: not_reached();
  }

#undef O

#undef D
#undef DofS
#undef DUnbox
#undef DBox
#undef DRefineS
#undef DParam
#undef DAllocObj
#undef DArrElem
#undef DArrPacked
#undef DLdRef
#undef DThis
#undef DMulti
#undef DStk
#undef DSetElem
#undef ND
#undef DBuiltin
#undef DSubtract
#undef DLdRaw
#undef DCns

}

//////////////////////////////////////////////////////////////////////

namespace {

// Returns a union type containing all the types in the
// variable-length argument list
Type buildUnion() {
  return Type::Bottom;
}

template<class... Args>
Type buildUnion(Type t, Args... ts) {
  return t | buildUnion(ts...);
}

}

/*
 * Runtime typechecking for IRInstruction operands.
 *
 * This is generated using the table in ir-opcode.h.  We instantiate
 * IR_OPCODES after defining all the various source forms to do type
 * assertions according to their form (see ir-opcode.h for documentation on
 * the notation).  The checkers appear in argument order, so each one
 * increments curSrc, and at the end we can check that the argument
 * count was also correct.
 */
bool checkOperandTypes(const IRInstruction* inst, const IRUnit* unit) {
  int curSrc = 0;

  auto bail = [&] (const std::string& msg) {
    FTRACE(1, "{}", msg);
    fprintf(stderr, "%s\n", msg.c_str());
    if (unit) print(*unit);
    always_assert(false && "instruction operand type check failure");
  };

  if (opHasExtraData(inst->op()) != (bool)inst->rawExtra()) {
    bail(folly::format("opcode {} should{} have an ExtraData struct "
                       "but instruction {} does{}",
                       inst->op(),
                       opHasExtraData(inst->op()) ? "" : "n't",
                       *inst,
                       inst->rawExtra() ? "" : "n't").str());
  }

  auto src = [&]() -> SSATmp* {
    if (curSrc < inst->numSrcs()) {
      return inst->src(curSrc);
    }

    bail(folly::format(
      "Error: instruction had too few operands\n"
      "   instruction: {}\n",
        inst->toString()
      ).str()
    );
    not_reached();
  };

  // If expected is not nullptr, it will be used. Otherwise, t.toString() will
  // be used as the expected string.
  auto check = [&] (bool cond, const Type t, const char* expected) {
    if (cond) return true;

    std::string expectStr = expected ? expected : t.toString();

    bail(folly::format(
      "Error: failed type check on operand {}\n"
      "   instruction: {}\n"
      "   was expecting: {}\n"
      "   received: {}\n",
        curSrc,
        inst->toString(),
        expectStr,
        inst->src(curSrc)->type().toString()
      ).str()
    );
    return true;
  };

  auto checkNoArgs = [&]{
    if (inst->numSrcs() == 0) return true;
    bail(folly::format(
      "Error: instruction expected no operands\n"
      "   instruction: {}\n",
        inst->toString()
      ).str()
    );
    return true;
  };

  auto countCheck = [&]{
    if (inst->numSrcs() == curSrc) return true;
    bail(folly::format(
      "Error: instruction had too many operands\n"
      "   instruction: {}\n"
      "   expected {} arguments\n",
        inst->toString(),
        curSrc
      ).str()
    );
    return true;
  };

  auto checkDst = [&] (bool cond, const std::string& errorMessage) {
    if (cond) return true;

    bail(folly::format("Error: failed type check on dest operand\n"
                       "   instruction: {}\n"
                       "   message: {}\n",
                       inst->toString(),
                       errorMessage).str());
    return true;
  };

  auto requireTypeParam = [&] {
    checkDst(inst->hasTypeParam() || inst->is(DefConst),
             "Invalid paramType for DParam instruction");
    if (inst->hasTypeParam()) {
      checkDst(inst->typeParam() != Type::Bottom,
             "Invalid paramType for DParam instruction");
    }
  };

  auto checkVariadic = [&] (Type super) {
    for (; curSrc < inst->numSrcs(); ++curSrc) {
      auto const valid = (inst->src(curSrc)->type() <= super);
      check(valid, Type(), nullptr);
    }
  };

#define IRT(name, ...) UNUSED static const Type name = Type::name;
  IR_TYPES
#undef IRT

#define NA            return checkNoArgs();
#define S(...)        {                                   \
                        Type t = buildUnion(__VA_ARGS__); \
                        check(src()->isA(t), t, nullptr); \
                        ++curSrc;                         \
                      }
#define AK(kind)      {                                                 \
                        Type t = Type::Arr.specialize(                  \
                          ArrayData::k##kind##Kind);                    \
                        check(src()->isA(t), t, nullptr);               \
                        ++curSrc;                                       \
                      }
#define C(type)       check(src()->isConst() && \
                            src()->isA(type),   \
                            Type(),             \
                            "constant " #type); \
                      ++curSrc;
#define CStr          C(StaticStr)
#define SVar(...)     checkVariadic(buildUnion(__VA_ARGS__));
#define ND
#define DMulti
#define DStk(...)
#define DSetElem
#define D(...)
#define DBuiltin
#define DSubtract(src, t)checkDst(src < inst->numSrcs(),  \
                             "invalid src num");
#define DUnbox(src) checkDst(src < inst->numSrcs(),  \
                             "invalid src num");
#define DBox(src)   checkDst(src < inst->numSrcs(),  \
                             "invalid src num");
#define DofS(src)   checkDst(src < inst->numSrcs(),  \
                             "invalid src num");
#define DRefineS(src) checkDst(src < inst->numSrcs(),  \
                               "invalid src num");     \
                      requireTypeParam();
#define DParam      requireTypeParam();
#define DLdRef      requireTypeParam();
#define DAllocObj
#define DArrElem
#define DArrPacked
#define DThis
#define DLdRaw
#define DCns

#define O(opcode, dstinfo, srcinfo, flags)      \
  case opcode: dstinfo srcinfo countCheck(); return true;

  switch (inst->op()) {
    IR_OPCODES
  default: always_assert(false);
  }

#undef O

#undef NA
#undef S
#undef AK
#undef C
#undef CStr
#undef SVar

#undef ND
#undef D
#undef DBuiltin
#undef DSubtract
#undef DUnbox
#undef DMulti
#undef DStk
#undef DSetElem
#undef DBox
#undef DofS
#undef DRefineS
#undef DParam
#undef DAllocObj
#undef DArrElem
#undef DArrPacked
#undef DLdRef
#undef DThis
#undef DLdRaw
#undef DCns

  return true;
}

std::string TypeConstraint::toString() const {
  std::string ret = "<" + typeCategoryName(category);

  if (innerCat > DataTypeGeneric) {
    folly::toAppend(",inner:", typeCategoryName(innerCat), &ret);
  }

  if (category == DataTypeSpecialized) {
    if (wantArrayKind()) ret += ",ArrayKind";
    if (wantClass()) {
      folly::toAppend("Cls:", desiredClass()->name()->data(), &ret);
    }
  }

  if (weak) ret += ",weak";

  return ret + '>';
}

//////////////////////////////////////////////////////////////////////

}}
