/*
   +----------------------------------------------------------------------+
   | HipHop for PHP                                                       |
   +----------------------------------------------------------------------+
   | Copyright (c) 2010-2013 Facebook, Inc. (http://www.facebook.com)     |
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

#ifndef incl_HPHP_JIT_VASM_X64_H_
#define incl_HPHP_JIT_VASM_X64_H_

#include "hphp/runtime/base/stats.h"
#include "hphp/runtime/base/types.h"
#include "hphp/runtime/vm/jit/abi.h"
#include "hphp/runtime/vm/jit/containers.h"
#include "hphp/runtime/vm/jit/cpp-call.h"
#include "hphp/runtime/vm/jit/fixup.h"
#include "hphp/runtime/vm/jit/phys-reg.h"
#include "hphp/runtime/vm/jit/service-requests.h"
#include "hphp/runtime/vm/jit/target-cache.h"
#include "hphp/runtime/vm/jit/vasm.h"
#include "hphp/runtime/vm/srckey.h"
#include "hphp/util/asm-x64.h"

#include <bitset>
#include <boost/dynamic_bitset.hpp>
#include <folly/Range.h>

namespace HPHP { namespace jit {
struct IRInstruction;
struct AsmInfo;
}}

namespace HPHP { namespace jit {

// XXX: This should go back to arg-group.h as part of work on t5297892
enum class DestType : uint8_t {
  None,  // return void (no valid registers)
  SSA,   // return a single-register value
  TV,    // return a TypedValue packed in two registers
  Dbl,   // return scalar double in a single FP register
  SIMD,  // return a TypedValue in one SIMD register
};
const char* destTypeName(DestType);

struct Vptr;
struct Vscaled;

// Vreg is like physreg, but numbers go beyond the physical register names.
// Since it is unconstrained, it has predicates to test whether rn is
// a gpr, xmm, or virtual register.
struct Vreg {
  static constexpr auto kind = VregKind::Any;
  static const unsigned kNumGP{PhysReg::kSIMDOffset}; // 33
  static const unsigned kNumXMM{30};
  static const unsigned kNumSF{1};
  static const unsigned G0{0};
  static const unsigned X0{kNumGP};
  static const unsigned S0{X0+kNumXMM};
  static const unsigned V0{S0+kNumSF};
  static const unsigned kInvalidReg{0xffffffffU};
  Vreg() : rn(kInvalidReg) {}
  explicit Vreg(size_t r) : rn(r) {}
  /* implicit */ Vreg(Reg64 r) : rn(int(r)) {}
  /* implicit */ Vreg(Reg32 r) : rn(int(r)) {}
  /* implicit */ Vreg(Reg8 r) : rn(int(r)) {}
  /* implicit */ Vreg(RegXMM r) : rn(X0+int(r)) {}
  /* implicit */ Vreg(RegSF r) : rn(S0+int(r)) {}
  /* implicit */ Vreg(PhysReg r) {
    rn = (r == InvalidReg) ? kInvalidReg :
         r.isGP() ? G0+int(Reg64(r)) :
          r.isSIMD() ? X0+int(RegXMM(r)) :
          /* r.isSF() ? */ S0+int(RegSF(r));
  }
  /* implicit */ operator size_t() const { return rn; }
  /* implicit */ operator Reg64() const {
    assert(isGP());
    return Reg64(rn - G0);
  }
  /* implicit */ operator RegXMM() const {
    assert(isSIMD());
    return RegXMM(rn - X0);
  }
  /* implicit */ operator RegSF() const {
    assert(isSF());
    return RegSF(rn - S0);
  }
  /* implicit */ operator PhysReg() const { return physReg(); }
  bool isPhys() const {
    static_assert(G0 < V0 && X0 < V0 && S0 < V0 && V0 < kInvalidReg, "");
    return rn < V0;
  }
  bool isGP() const { return /* rn >= G0 && */ rn < G0+kNumGP; }
  bool isSIMD() const { return rn >= X0 && rn < X0+kNumXMM; }
  bool isSF() const { return rn >= S0 && rn < S0+kNumSF; }
  bool isVirt() const { return rn >= V0 && isValid(); }
  bool isValid() const { return rn != kInvalidReg; }
  bool operator==(Vreg r) const { return rn == r.rn; }
  bool operator!=(Vreg r) const { return rn != r.rn; }
  PhysReg physReg() const {
    assert(!isValid() || isPhys());
    return !isValid() ? InvalidReg :
           isGP() ? PhysReg(/* implicit */operator Reg64()) :
           isSIMD() ? PhysReg(/* implicit */operator RegXMM()) :
           /* isSF() ? */ PhysReg(/* implicit */operator RegSF());
  }
  Vptr operator[](int disp) const;
  Vptr operator[](ScaledIndex) const;
  Vptr operator[](ScaledIndexDisp) const;
  Vptr operator[](Vptr) const;
  Vptr operator[](DispReg) const;
  Vptr operator*() const;
  Vptr operator[](Vscaled) const;
  Vscaled operator*(int scale) const;
  Vptr operator[](Vreg) const;
  Vptr operator+(size_t d) const;
private:
  unsigned rn{kInvalidReg};
};

// instantiations of this wrap virtual register numbers in in a strongly
// typed wrapper that conveys physical constraints, similar to Reg64,
// Reg32, RegXMM, etc.
template<class Reg, VregKind Kind, int Bits> struct Vr {
  static constexpr auto bits = Bits;
  static constexpr auto kind = Kind;
  explicit Vr(size_t rn) : rn(rn) {}
  /* implicit */ Vr(Vreg r) : rn(size_t(r)) {
    if (kind == VregKind::Gpr) {
      assert(!r.isValid() || r.isVirt() || r.isGP());
    } else if (kind == VregKind::Simd) {
      assert(!r.isValid() || r.isVirt() || r.isSIMD());
    } else if (kind == VregKind::Sf) {
      assert(!r.isValid() || r.isVirt() || r.isSF());
    }
  }
  /* implicit */ Vr(Reg r) : Vr{Vreg(r)} {}
  /* implicit */ Vr(PhysReg pr) : Vr{Vreg(pr)} {}
  Reg asReg() const {
    assert(isPhys());
    return isGP() ? Reg(rn) :
           isSIMD() ? Reg(rn-Vreg::X0) :
           /* isSF() ? */ Reg(rn-Vreg::S0);
  }
  /* implicit */ operator Reg() const { return asReg(); }
  /* implicit */ operator Vreg() const { return Vreg(rn); }
  /* implicit */ operator size_t() const { return rn; }
  bool isPhys() const {
    static_assert(Vreg::G0 == 0, "");
    return rn < Vreg::V0;
  }
  bool isGP() const { return rn>=Vreg::G0 && rn<Vreg::G0+Vreg::kNumGP; }
  bool isSIMD() const { return rn>=Vreg::X0 && rn<Vreg::X0+Vreg::kNumXMM; }
  bool isSF() const { return rn>=Vreg::S0 && rn<Vreg::S0+Vreg::kNumSF; }
  bool isVirt() const { return rn >= Vreg::V0 && isValid(); }
  bool operator==(Vr<Reg,Kind,Bits> r) const { return rn == r.rn; }
  bool operator!=(Vr<Reg,Kind,Bits> r) const { return rn != r.rn; }
  bool operator==(PhysReg) const = delete;
  bool operator!=(PhysReg) const = delete;
  bool isValid() const { return rn != Vreg::kInvalidReg; }
  Vptr operator[](int disp) const;
  Vptr operator[](ScaledIndex si) const;
  Vptr operator[](ScaledIndexDisp sid) const;
  Vptr operator[](Vptr) const;
  Vptr operator[](DispReg) const;
  Vptr operator*() const;
  Vptr operator+(size_t d) const;
  explicit operator PhysReg() const { return asReg(); }
private:
  unsigned rn;
};
typedef Vr<Reg64,VregKind::Gpr,64>    Vreg64;
typedef Vr<Reg32,VregKind::Gpr,32>    Vreg32;
typedef Vr<Reg16,VregKind::Gpr,16>    Vreg16;
typedef Vr<Reg8,VregKind::Gpr,8>      Vreg8;
typedef Vr<RegXMM,VregKind::Simd,64>  VregDbl;
typedef Vr<RegXMM,VregKind::Simd,128> Vreg128;
typedef Vr<RegSF,VregKind::Sf,4>      VregSF;

inline Reg64 r64(Vreg64 r) { return r; }

// base + index*scale + disp.
// base is optional, implying baseless address
// index is optional
struct Vptr {
  enum Segment: uint8_t { DS, FS };
  template<class Base> Vptr(Base b, int d)
    : base(b), index(0xffffffff), scale(1), disp(d)
  {}
  template<class Base, class Index> Vptr(Base b, Index i, int s, int d)
    : base(b), index(i), scale(s), disp(d)
  {}
  /* implicit */ Vptr(MemoryRef m, Segment s = DS)
    : base(m.r.base), index(m.r.index), scale(m.r.scale)
    , seg(s), disp(m.r.disp)
  {}
  MemoryRef mr() const {
    if (index.isValid()) {
      return base.isValid() ? r64(base)[r64(index) * scale + disp] :
             *(IndexedDispReg{r64(index) * scale + disp});
    } else {
      return base.isValid() ? r64(base)[disp] :
             *(DispReg{disp});
    }
  }
  /* implicit */ operator MemoryRef() const {
    assert(seg == DS);
    return mr();
  }

  Vreg64 base; // optional, for baseless mode
  Vreg64 index; // optional
  uint8_t scale; // 1,2,4,8
  Segment seg{DS}; // DS or FS
  int32_t disp;
};

struct Vscaled {
  Vreg64 index;
  int scale;
};

inline Vptr operator+(Vptr lhs, int32_t d) {
  return Vptr(lhs.base, lhs.index, lhs.scale, lhs.disp + d);
}

inline Vptr operator+(Vptr lhs, intptr_t d) {
  return Vptr(lhs.base, lhs.index, lhs.scale,
              safe_cast<int32_t>(lhs.disp + d));
}

inline Vptr operator+(Vreg64 base, int32_t d) {
  return Vptr(base, d);
}

// A Vloc is either a single or pair of vregs, for keeping track
// of where we have stored an SSATmp.
struct Vloc {
  enum Kind { kPair, kWide };
  Vloc() {}
  explicit Vloc(Vreg r) { m_regs[0] = r; }
  Vloc(Vreg r0, Vreg r1) { m_regs[0] = r0; m_regs[1] = r1; }
  Vloc(Kind kind, Vreg r) : m_kind(kind) { m_regs[0] = r; }
  bool hasReg(int i = 0) const {
    return m_regs[i].isValid();
  }
  Vreg reg(int i = 0) const {
    return m_regs[i];
  }
  int numAllocated() const {
    return int(m_regs[0].isValid()) + int(m_regs[1].isValid());
  }
  int numWords() const {
    return m_kind == kWide ? 2 : numAllocated();
  }
  bool isFullSIMD() const {
    return m_kind == kWide;
  }

  bool operator==(Vloc other) const {
    return m_kind == other.m_kind &&
           m_regs[0] == other.m_regs[0] &&
           m_regs[1] == other.m_regs[1];
  }
  bool operator!=(Vloc other) const {
    return !(*this == other);
  }

private:
  Kind m_kind{kPair};
  Vreg m_regs[2];
};

inline Vscaled Vreg::operator*(int scale) const {
  return Vscaled{*this, scale};
}

inline Vptr Vreg::operator[](Vscaled si) const {
  return Vptr(*this, si.index, si.scale, 0);
}

inline Vptr Vreg::operator*() const { return Vptr(*this, 0); }
inline Vptr Vreg::operator[](int disp) const { return Vptr(*this, disp); }
inline Vptr Vreg::operator[](ScaledIndex si) const {
  return Vptr(*this, si.index, si.scale, 0);
}
inline Vptr Vreg::operator[](ScaledIndexDisp sid) const {
  return Vptr(*this, sid.si.index, sid.si.scale, sid.disp);
}
inline Vptr Vreg::operator[](Vptr p) const {
  return Vptr(*this, p.base, 1, p.disp);
}
inline Vptr Vreg::operator[](DispReg rd) const {
  return Vptr(*this, rd.base, 1, rd.disp);
}
inline Vptr Vreg::operator[](Vreg index) const {
  return Vptr(*this, index, 1, 0);
}

template<class Reg, VregKind Kind, int Bits>
Vptr Vr<Reg,Kind,Bits>::operator*() const {
  return Vptr(*this, 0);
}

template<class Reg, VregKind Kind, int Bits>
Vptr Vr<Reg,Kind,Bits>::operator[](int disp) const {
  return Vptr(*this, disp);
}

inline Vptr operator+(Vreg base, int32_t d) {
  return Vptr(base, safe_cast<int32_t>(d));
}

inline Vptr operator+(Vreg base, intptr_t d) {
  return Vptr(base, safe_cast<int32_t>(d));
}

inline Vptr Vreg::operator+(size_t d) const {
  return Vptr(*this, safe_cast<int32_t>(d));
}

inline Vptr operator+(Vreg64 base, intptr_t d) {
  return Vptr(base, safe_cast<int32_t>(d));
}

template<class Reg, VregKind Kind, int Bits>
Vptr Vr<Reg,Kind,Bits>::operator[](ScaledIndex si) const {
  return Vptr(*this, si.index, si.scale, 0);
}

template<class Reg, VregKind Kind, int Bits>
Vptr Vr<Reg,Kind,Bits>::operator[](ScaledIndexDisp sid) const {
  return Vptr(*this, sid.si.index, sid.si.scale, sid.disp);
}

template<class Reg, VregKind Kind, int Bits>
Vptr Vr<Reg,Kind,Bits>::operator[](Vptr p) const {
  return Vptr(*this, p.base, 1, p.disp);
}

template<class Reg, VregKind Kind, int Bits>
Vptr Vr<Reg,Kind,Bits>::operator[](DispReg rd) const {
  return Vptr(*this, rd.base, 1, rd.disp);
}

template<class Reg, VregKind Kind, int Bits>
inline Vptr Vr<Reg,Kind,Bits>::operator+(size_t d) const {
  return Vptr(*this, safe_cast<int32_t>(d));
}

// Field actions:
// I(f)     immediate
// Inone    no immediates
// U(s)     use s
// UA(s)    use s, but s lifetime extends across the instruction
// UH(s,h)  use s, try assigning same register as h
// D(d)     define d
// DH(d,h)  define d, try assigning same register as h
// Un,Dn    no uses, defs

#define X64_OPCODES\
  /* intrinsics */\
  O(bindaddr, I(dest) I(sk), Un, Dn)\
  O(bindcall, I(sk) I(callee) I(argc), Un, Dn)\
  O(bindexit, I(cc) I(target), U(sf), Dn)\
  O(bindjcc1, I(cc) I(targets[0]) I(targets[1]), U(sf), Dn)\
  O(bindjcc2, I(cc) I(target), U(sf), Dn)\
  O(bindjmp, I(target) I(trflags), Un, Dn)\
  O(callstub, I(target) I(kills) I(fix), U(args), Dn)\
  O(contenter, Inone, U(fp) U(target), Dn)\
  O(copy, Inone, UH(s,d), DH(d,s))\
  O(copy2, Inone, UH(s0,d0) UH(s1,d1), DH(d0,s0) DH(d1,s1))\
  O(copyargs, Inone, UH(s,d), DH(d,s))\
  O(end, Inone, Un, Dn)\
  O(ldimm, I(s) I(saveflags), Un, D(d))\
  O(fallback, I(dest), Un, Dn)\
  O(fallbackcc, I(cc) I(dest), U(sf), Dn)\
  O(incstat, I(stat) I(n) I(force), Un, Dn)\
  O(kpcall, I(target) I(callee) I(prologIndex), Un, Dn)\
  O(ldpoint, I(s), Un, D(d))\
  O(load, Inone, U(s), D(d))\
  O(mccall, I(target), U(args), Dn)\
  O(mcprep, Inone, Un, D(d))\
  O(nop, Inone, Un, Dn)\
  O(nothrow, Inone, Un, Dn)\
  O(phidef, Inone, Un, D(defs))\
  O(phijmp, Inone, U(uses), Dn)\
  O(point, I(p), Un, Dn)\
  O(resume, Inone, Un, Dn)\
  O(retransopt, I(sk) I(id), Un, Dn)\
  O(store, Inone, U(s) U(d), Dn)\
  O(syncpoint, I(fix), Un, Dn)\
  O(unwind, Inone, Un, Dn)\
  O(vcall, I(call) I(destType) I(fixup), U(args), D(d))\
  O(vinvoke, I(call) I(destType) I(fixup), U(args), D(d))\
  O(landingpad, Inone, Un, Dn)\
  /* arm instructions */\
  O(asrv, Inone, U(sl) U(sr), D(d))\
  O(brk, I(code), Un, Dn)\
  O(cbcc, I(cc), U(s), Dn)\
  O(hcsync, I(fix) I(call), Un, Dn)\
  O(hcnocatch, I(call), Un, Dn)\
  O(hcunwind, I(call), Un, Dn)\
  O(hostcall, I(argc) I(syncpoint), U(args), Dn)\
  O(lslv, Inone, U(sl) U(sr), D(d))\
  O(tbcc, I(cc) I(bit), U(s), Dn)\
  /* x64 instructions */\
  O(addli, I(s0), UH(s1,d), DH(d,s1) D(sf)) \
  O(addlm, Inone, U(s0) U(m), D(sf)) \
  O(addq, Inone, U(s0) U(s1), D(d) D(sf)) \
  O(addqi, I(s0), UH(s1,d), DH(d,s1) D(sf)) \
  O(addsd, Inone, U(s0) U(s1), D(d))\
  O(andb, Inone, U(s0) U(s1), D(d) D(sf)) \
  O(andbi, I(s0), UH(s1,d), DH(d,s1) D(sf)) \
  O(andbim, I(s), U(m), D(sf)) \
  O(andl, Inone, U(s0) U(s1), D(d) D(sf)) \
  O(andli, I(s0), UH(s1,d), DH(d,s1) D(sf)) \
  O(andq, Inone, U(s0) U(s1), D(d) D(sf)) \
  O(andqi, I(s0), UH(s1,d), DH(d,s1) D(sf)) \
  O(call, I(target), U(args), Dn)\
  O(callm, Inone, U(target) U(args), Dn)\
  O(callr, Inone, U(target) U(args), Dn)\
  O(cloadq, I(cc), U(sf) U(f) U(t), D(d))\
  O(cmovq, I(cc), U(sf) U(f) U(t), D(d))\
  O(cmpb, Inone, U(s0) U(s1), D(sf))\
  O(cmpbi, I(s0), U(s1), D(sf))\
  O(cmpbim, I(s0), U(s1), D(sf))\
  O(cmpl, Inone, U(s0) U(s1), D(sf))\
  O(cmpli, I(s0), U(s1), D(sf))\
  O(cmplim, I(s0), U(s1), D(sf))\
  O(cmplm, Inone, U(s0) U(s1), D(sf))\
  O(cmpq, Inone, U(s0) U(s1), D(sf))\
  O(cmpqi, I(s0), U(s1), D(sf))\
  O(cmpqim, I(s0), U(s1), D(sf))\
  O(cmpqm, Inone, U(s0) U(s1), D(sf))\
  O(cmpsd, I(pred), UA(s0) U(s1), D(d))\
  O(cqo, Inone, Un, Dn)\
  O(cvttsd2siq, Inone, U(s), D(d))\
  O(cvtsi2sd, Inone, U(s), D(d))\
  O(cvtsi2sdm, Inone, U(s), D(d))\
  O(decl, Inone, UH(s,d), DH(d,s) D(sf))\
  O(declm, Inone, U(m), D(sf))\
  O(decq, Inone, UH(s,d), DH(d,s) D(sf))\
  O(decqm, Inone, U(m), D(sf))\
  O(divsd, Inone, UA(s0) U(s1), D(d))\
  O(incwm, Inone, U(m), D(sf))\
  O(idiv, Inone, U(s), D(sf))\
  O(imul, Inone, U(s0) U(s1), D(d) D(sf))\
  O(incl, Inone, UH(s,d), DH(d,s) D(sf))\
  O(inclm, Inone, U(m), D(sf))\
  O(incq, Inone, UH(s,d), DH(d,s) D(sf))\
  O(incqm, Inone, U(m), D(sf))\
  O(incqmlock, Inone, U(m), D(sf))\
  O(jcc, I(cc), U(sf), Dn)\
  O(jmp, Inone, Un, Dn)\
  O(jmpr, Inone, U(target), Dn)\
  O(jmpm, Inone, U(target), Dn)\
  O(lea, Inone, U(s), D(d))\
  O(leap, I(s), Un, D(d))\
  O(loaddqu, Inone, U(s), D(d))\
  O(loadl, Inone, U(s), D(d))\
  O(loadq, Inone, U(s), D(d))\
  O(loadqp, I(s), Un, D(d))\
  O(loadsd, Inone, U(s), D(d))\
  O(loadzbl, Inone, U(s), D(d))\
  O(movb, Inone, UH(s,d), DH(d,s))\
  O(movbi, I(s), Un, D(d))\
  O(movl, Inone, UH(s,d), DH(d,s))\
  O(movsbl, Inone, UH(s,d), DH(d,s))\
  O(movzbl, Inone, UH(s,d), DH(d,s))\
  O(mulsd, Inone, U(s0) U(s1), D(d))\
  O(neg, Inone, UH(s,d), DH(d,s) D(sf))\
  O(not, Inone, UH(s,d), DH(d,s))\
  O(orq, Inone, U(s0) U(s1), D(d) D(sf))\
  O(orqi, I(s0), UH(s1,d), DH(d,s1) D(sf)) \
  O(orqim, I(s0), U(m), D(sf))\
  O(pop, Inone, Un, D(d))\
  O(popm, Inone, U(m), Dn)\
  O(psllq, I(s0), UH(s1,d), DH(d,s1))\
  O(psrlq, I(s0), UH(s1,d), DH(d,s1))\
  O(push, Inone, U(s), Dn)\
  O(pushl, Inone, U(s), Dn)\
  O(pushm, Inone, U(s), Dn)\
  O(ret, Inone, Un, Dn)\
  O(roundsd, I(dir), U(s), D(d))\
  O(sarq, Inone, U(s), D(d) D(sf))\
  O(sarqi, I(s0), UH(s1,d), DH(d,s1) D(sf))\
  O(sbbl, Inone, U(sfu) UA(s0) U(s1), D(d) D(sfd))\
  O(setcc, I(cc), U(sf), D(d))\
  O(shlli, I(s0), UH(s1,d), DH(d,s1) D(sf))\
  O(shlq, Inone, U(s), D(d) D(sf))\
  O(shlqi, I(s0), UH(s1,d), DH(d,s1) D(sf))\
  O(shrli, I(s0), UH(s1,d), DH(d,s1) D(sf))\
  O(shrqi, I(s0), UH(s1,d), DH(d,s1) D(sf))\
  O(sqrtsd, Inone, U(s), D(d))\
  O(storeb, Inone, U(s) U(m), Dn)\
  O(storebim, I(s), U(m), Dn)\
  O(storedqu, Inone, U(s) U(m), Dn)\
  O(storel, Inone, U(s) U(m), Dn)\
  O(storelim, I(s), U(m), Dn)\
  O(storeq, Inone, U(s) U(m), Dn)\
  O(storeqim, I(s), U(m), Dn)\
  O(storew, Inone, U(s) U(m), Dn)\
  O(storesd, Inone, U(s) U(m), Dn)\
  O(storewim, I(s), U(m), Dn)\
  O(subl, Inone, UA(s0) U(s1), D(d) D(sf))\
  O(subli, I(s0), UH(s1,d), DH(d,s1) D(sf))\
  O(subq, Inone, UA(s0) U(s1), D(d) D(sf))\
  O(subqi, I(s0), UH(s1,d), DH(d,s1) D(sf))\
  O(subsd, Inone, UA(s0) U(s1), D(d))\
  O(testb, Inone, U(s0) U(s1), D(sf))\
  O(testbi, I(s0), U(s1), D(sf))\
  O(testbim, I(s0), U(s1), D(sf))\
  O(testl, Inone, U(s0) U(s1), D(sf))\
  O(testli, I(s0), U(s1), D(sf))\
  O(testlim, I(s0), U(s1), D(sf))\
  O(testq, Inone, U(s0) U(s1), D(sf))\
  O(testqm, Inone, U(s0) U(s1), D(sf))\
  O(testqim, I(s0), U(s1), D(sf))\
  O(ucomisd, Inone, U(s0) U(s1), D(sf))\
  O(ud2, Inone, Un, Dn)\
  O(unpcklpd, Inone, UA(s0) U(s1), D(d))\
  O(xorb, Inone, U(s0) U(s1), D(d) D(sf))\
  O(xorbi, I(s0), UH(s1,d), DH(d,s1) D(sf))\
  O(xorq, Inone, U(s0) U(s1), D(d) D(sf))\
  O(xorqi, I(s0), UH(s1,d), DH(d,s1) D(sf))\

// intrinsics
struct bindaddr { TCA* dest; SrcKey sk; };
struct bindcall { SrcKey sk; const Func* callee; unsigned argc; };
struct bindexit { ConditionCode cc; VregSF sf; SrcKey target;
                  TransFlags trflags; };
struct bindjcc1 { ConditionCode cc; VregSF sf; Offset targets[2]; };
struct bindjcc2 { ConditionCode cc; VregSF sf; Offset target; };
struct bindjmp { SrcKey target; TransFlags trflags; };
struct vcall { CppCall call; VcallArgsId args; Vtuple d;
               Fixup fixup; DestType destType; bool nothrow; };
struct vinvoke { CppCall call; VcallArgsId args; Vtuple d; Vlabel targets[2];
                 Fixup fixup; DestType destType; bool smashable;};
struct callstub { CodeAddress target; RegSet args, kills; Fixup fix; };
struct contenter { Vreg64 fp, target; };
struct copy { Vreg s, d; };
struct copy2 { Vreg64 s0, s1, d0, d1; };
struct copyargs { Vtuple s, d; };
struct end {};
struct ldimm { Immed64 s; Vreg d; bool saveflags; };
struct fallback { SrcKey dest; TransFlags trflags; };
struct fallbackcc { ConditionCode cc; VregSF sf; SrcKey dest;
                    TransFlags trflags; };
struct incstat { Stats::StatCounter stat; int n; bool force; };
struct kpcall { CodeAddress target; const Func* callee; unsigned prologIndex; };
struct ldpoint { Vpoint s; Vreg64 d; };
struct load { Vptr s; Vreg d; };
struct mccall { CodeAddress target; RegSet args; };
struct mcprep { Vreg64 d; };
struct nop {};
struct nothrow {};
struct phidef { Vtuple defs; };
struct phijmp { Vlabel target; Vtuple uses; };
struct point { Vpoint p; };
struct resume {};
struct retransopt { SrcKey sk; TransID id; };
struct store { Vreg s; Vptr d; };
struct syncpoint { Fixup fix; };
struct unwind { Vlabel targets[2]; };
struct landingpad {};

// arm-specific intrinsics
struct hcsync { Fixup fix; Vpoint call; };
struct hcnocatch { Vpoint call; };
struct hcunwind { Vpoint call; Vlabel targets[2]; };

// arm specific instructions
struct brk { uint16_t code; };
struct hostcall { RegSet args; uint8_t argc; Vpoint syncpoint; };
struct cbcc { vixl::Condition cc; Vreg64 s; Vlabel targets[2]; };
struct tbcc { vixl::Condition cc; unsigned bit; Vreg64 s; Vlabel targets[2]; };
struct lslv { Vreg64 sl, sr, d; };
struct asrv { Vreg64 sl, sr, d; };

// ATT style operand order. for binary ops:
// op   s0 s1 d:  d = s1 op s0    =>   d=s1; d op= s0
// op   imm s1 d: d = s1 op imm   =>   d=s1; d op= imm
// cmp  s0 s1:    s1 cmp s0

// suffix conventions:
//  b   8-bit
//  w   16-bit
//  l   32-bit
//  q   64-bit
//  i   immediate
//  m   Vptr
//  p   RIPRelativeRef

// x64 instructions
struct addli { Immed s0; Vreg32 s1, d; VregSF sf; };
struct addlm { Vreg32 s0; Vptr m; VregSF sf; };
struct addq  { Vreg64 s0, s1, d; VregSF sf; };
struct addqi { Immed s0; Vreg64 s1, d; VregSF sf; };
struct addsd  { VregDbl s0, s1, d; };
struct andb  { Vreg8 s0, s1, d; VregSF sf; };
struct andbi { Immed s0; Vreg8 s1, d; VregSF sf; };
struct andbim { Immed s; Vptr m; VregSF sf; };
struct andl  { Vreg32 s0, s1, d; VregSF sf; };
struct andli { Immed s0; Vreg32 s1, d; VregSF sf; };
struct andq  { Vreg64 s0, s1, d; VregSF sf; };
struct andqi { Immed s0; Vreg64 s1, d; VregSF sf; };
struct call { CodeAddress target; RegSet args; };
struct callm { Vptr target; RegSet args; };
struct callr { Vreg64 target; RegSet args; };
struct cloadq { ConditionCode cc; VregSF sf; Vreg64 f; Vptr t; Vreg64 d; };
struct cmovq { ConditionCode cc; VregSF sf; Vreg64 f, t, d; };
struct cmpb  { Vreg8  s0; Vreg8  s1; VregSF sf; };
struct cmpbi { Immed  s0; Vreg8  s1; VregSF sf; };
struct cmpbim { Immed s0; Vptr s1; VregSF sf; };
struct cmpl  { Vreg32 s0; Vreg32 s1; VregSF sf; };
struct cmpli { Immed  s0; Vreg32 s1; VregSF sf; };
struct cmplim { Immed s0; Vptr s1; VregSF sf; };
struct cmplm { Vreg32 s0; Vptr s1; VregSF sf; };
struct cmpq  { Vreg64 s0; Vreg64 s1; VregSF sf; };
struct cmpqi { Immed  s0; Vreg64 s1; VregSF sf; };
struct cmpqim { Immed s0; Vptr s1; VregSF sf; };
struct cmpqm { Vreg64 s0; Vptr s1; VregSF sf; };
struct cmpsd { ComparisonPred pred; VregDbl s0, s1, d; };
struct cqo {};
struct cvttsd2siq { VregDbl s; Vreg64 d; };
struct cvtsi2sd { Vreg64 s; VregDbl d; };
struct cvtsi2sdm { Vptr s; VregDbl d; };
struct decl { Vreg32 s, d; VregSF sf; };
struct declm { Vptr m; VregSF sf; };
struct decq { Vreg64 s, d; VregSF sf; };
struct decqm { Vptr m; VregSF sf; };
struct divsd { VregDbl s0, s1, d; };
struct idiv { Vreg64 s; VregSF sf; };
struct imul { Vreg64 s0, s1, d; VregSF sf; };
struct incl { Vreg32 s, d; VregSF sf; };
struct inclm { Vptr m; VregSF sf; };
struct incq { Vreg64 s, d; VregSF sf; };
struct incqm { Vptr m; VregSF sf; };
struct incqmlock { Vptr m; VregSF sf; };
struct incwm { Vptr m; VregSF sf; };
struct jcc { ConditionCode cc; VregSF sf; Vlabel targets[2]; };
struct jmp { Vlabel target; };
struct jmpr { Vreg64 target; };
struct jmpm { Vptr target; };
struct lea { Vptr s; Vreg64 d; };
struct leap { RIPRelativeRef s; Vreg64 d; };
struct loaddqu { Vptr s; Vreg128 d; };
struct loadl { Vptr s; Vreg32 d; };
struct loadq { Vptr s; Vreg64 d; };
struct loadqp { RIPRelativeRef s; Vreg64 d; };
struct loadsd { Vptr s; VregDbl d; };
struct loadzbl { Vptr s; Vreg32 d; };
struct movb { Vreg8 s, d; };
struct movbi { Immed s; Vreg8 d; };
struct movl { Vreg32 s, d; };
struct movsbl { Vreg8 s; Vreg32 d; };
struct movzbl { Vreg8 s; Vreg32 d; };
struct mulsd  { VregDbl s0, s1, d; };
struct neg { Vreg64 s, d; VregSF sf; };
struct not { Vreg64 s, d; };
struct orq { Vreg64 s0, s1, d; VregSF sf; };
struct orqi { Immed s0; Vreg64 s1, d; VregSF sf; };
struct orqim { Immed s0; Vptr m; VregSF sf; };
struct pop { Vreg64 d; };
struct popm { Vptr m; };
struct psllq { Immed s0; VregDbl s1, d; };
struct psrlq { Immed s0; VregDbl s1, d; };
struct push { Vreg64 s; };
struct pushl { Vreg32 s; };
struct pushm { Vptr s; };
struct ret {};
struct roundsd { RoundDirection dir; VregDbl s, d; };
struct sarq { Vreg64 s, d; VregSF sf; }; // uses rcx
struct sarqi { Immed s0; Vreg64 s1, d; VregSF sf; };
struct sbbl { VregSF sfu; Vreg32 s0, s1, d; VregSF sfd; };
struct setcc { ConditionCode cc; VregSF sf; Vreg8 d; };
struct shlli { Immed s0; Vreg32 s1, d; VregSF sf; };
struct shlq { Vreg64 s, d; VregSF sf; }; // uses rcx
struct shlqi { Immed s0; Vreg64 s1, d; VregSF sf; };
struct shrli { Immed s0; Vreg32 s1, d; VregSF sf; };
struct shrqi { Immed s0; Vreg64 s1, d; VregSF sf; };
struct sqrtsd { VregDbl s, d; };
struct storeb { Vreg8 s; Vptr m; };
struct storebim { Immed s; Vptr m; };
struct storedqu { Vreg128 s; Vptr m; };
struct storel { Vreg32 s; Vptr m; };
struct storelim { Immed s; Vptr m; };
struct storeq { Vreg64 s; Vptr m; };
struct storeqim { Immed s; Vptr m; };
struct storesd { VregDbl s; Vptr m; };
struct storew { Vreg16 s; Vptr m; };
struct storewim { Immed s; Vptr m; };
struct subl { Vreg32 s0, s1, d; VregSF sf; };
struct subli { Immed s0; Vreg32 s1, d; VregSF sf; };
struct subq { Vreg64 s0, s1, d; VregSF sf; };
struct subqi { Immed s0; Vreg64 s1, d; VregSF sf; };
struct subsd { VregDbl s0, s1, d; };
struct testb { Vreg8 s0, s1; VregSF sf; };
struct testbi { Immed s0; Vreg8 s1; VregSF sf; };
struct testbim { Immed s0; Vptr s1; VregSF sf; };
struct testl { Vreg32 s0, s1; VregSF sf; };
struct testli { Immed s0; Vreg32 s1; VregSF sf; };
struct testlim { Immed s0; Vptr s1; VregSF sf; };
struct testq { Vreg64 s0, s1; VregSF sf; };
struct testqm { Vreg64 s0; Vptr s1; VregSF sf; };
struct testqim { Immed s0; Vptr s1; VregSF sf; };
struct ucomisd { VregDbl s0, s1; VregSF sf; };
struct ud2 {};
struct unpcklpd { VregDbl s0, s1; Vreg128 d; };
struct xorb { Vreg8 s0, s1, d; VregSF sf; };
struct xorbi { Immed s0; Vreg8 s1, d; VregSF sf; };
struct xorq { Vreg64 s0, s1, d; VregSF sf; };
struct xorqi { Immed s0; Vreg64 s1, d; VregSF sf; };

struct Vinstr {
#define O(name, imms, uses, defs) name,
  enum Opcode : uint8_t { X64_OPCODES };
#undef O

  Vinstr()
    : op(ud2)
  {}

#define O(name, imms, uses, defs)                               \
  /* implicit */ Vinstr(jit::name i) : op(name), name##_(i) {}
  X64_OPCODES
#undef O

  Opcode op;

  /*
   * Instruction position, currently used only in vasm-xls.
   */
  unsigned pos;

  /*
   * If present, the IRInstruction this Vinstr was originally created from.
   */
  const IRInstruction* origin{nullptr};

  /*
   * A union of all possible instructions, descriminated by the op field.
   */
#define O(name, imms, uses, defs) jit::name name##_;
  union { X64_OPCODES };
#undef O
};

struct Vblock {
  explicit Vblock(AreaIndex area) : area(area) {}
  AreaIndex area;
  jit::vector<Vinstr> code;
};

typedef jit::vector<Vreg> VregList;

/*
 * Source operands for vcall/vinvoke instructions, packed into a struct for
 * convenience and to keep the instructions compact.
 */
struct VcallArgs {
  VregList args, simdArgs, stkArgs;
};

/*
 * A Vunit contains all the assets that make up a vasm compilation unit. It is
 * responsible for allocating new blocks, Vregs, and tuples.
 */
struct Vunit {
  /*
   * Create a new block in the given area, returning its id.
   */
  Vlabel makeBlock(AreaIndex area);

  /*
   * Create a block intended to be used temporarily, as part of modifying
   * existing code. Although not necessary for correctness, the block may be
   * freed with freeScratchBlock when finished.
   */
  Vlabel makeScratchBlock();

  /*
   * Free a scratch block when finished with it. There must be no references to
   * this block in reachable code.
   */
  void freeScratchBlock(Vlabel);

  Vreg makeReg() { return Vreg{next_vr++}; }
  Vtuple makeTuple(VregList&& regs);
  Vtuple makeTuple(const VregList& regs);
  VcallArgsId makeVcallArgs(VcallArgs&& args);

  Vreg makeConst(uint64_t);
  Vreg makeConst(double);
  Vreg makeConst(const void* p) { return makeConst(uint64_t(p)); }
  Vreg makeConst(uint32_t v) { return makeConst(uint64_t(v)); }
  Vreg makeConst(int64_t v) { return makeConst(uint64_t(v)); }
  Vreg makeConst(int32_t v) { return makeConst(int64_t(v)); }
  Vreg makeConst(DataType t) { return makeConst(uint64_t(t)); }
  Vreg makeConst(Immed64 v) { return makeConst(uint64_t(v.q())); }

  /*
   * Returns true iff this Vunit needs register allocation before it can be
   * emitted, either because it uses virtual registers or contains instructions
   * that must be lowered by xls.
   */
  bool needsRegAlloc() const;

  unsigned next_vr{Vreg::V0};
  Vlabel entry;
  jit::vector<Vblock> blocks;
  jit::hash_map<uint64_t,Vreg> cpool;
  jit::vector<VregList> tuples;
  jit::vector<VcallArgs> vcallArgs;
};

// writer stream to add instructions to a block
struct Vout {
  Vout(Vmeta* m, Vunit& u, Vlabel b, const IRInstruction* origin = nullptr)
    : m_meta(m), m_unit(u), m_block(b), m_origin(origin)
  {}

  Vout& operator=(const Vout& v) {
    assert(&v.m_unit == &m_unit && v.m_meta == m_meta);
    m_block = v.m_block;
    m_origin = v.m_origin;
    return *this;
  }

  // implicit cast to label for initializing branch instructions
  /* implicit */ operator Vlabel() const;
  bool empty() const;
  bool closed() const;

  Vout makeBlock(); // create a stream connected to a new empty block

  // instruction emitter
  Vout& operator<<(const Vinstr& inst);

  Vpoint makePoint() { return m_meta->makePoint(); }
  Vmeta& meta() { return *m_meta; }
  Vunit& unit() { return m_unit; }
  template<class T> Vreg cns(T v) { return m_unit.makeConst(v); }
  void use(Vlabel b) { m_block = b; }
  void setOrigin(const IRInstruction* i) { m_origin = i; }
  Vreg makeReg() { return m_unit.makeReg(); }
  AreaIndex area() const { return m_unit.blocks[m_block].area; }
  Vtuple makeTuple(const VregList& regs) const {
    return m_unit.makeTuple(regs);
  }
  Vtuple makeTuple(VregList&& regs) const {
    return m_unit.makeTuple(std::move(regs));
  }
  VcallArgsId makeVcallArgs(VcallArgs&& args) const {
    return m_unit.makeVcallArgs(std::move(args));
  }

private:
  Vmeta* const m_meta;
  Vunit& m_unit;
  Vlabel m_block;
  const IRInstruction* m_origin;
};

// Similar to X64Assembler, but buffers instructions as they
// are written, then generates code all at once at the end.
// Areas represent the separate sections we generate code into;
struct Vasm {
  struct Area {
    Vout out;
    CodeBlock& code;
    CodeAddress start;
  };
  typedef jit::vector<Area> AreaList;

  explicit Vasm(Vmeta* meta) : m_meta(meta) {
    m_areas.reserve(size_t(AreaIndex::Max));
  }

  void finishX64(const Abi&, AsmInfo* asmInfo);
  void finishARM(const Abi&, AsmInfo* asmInfo);

  // get an existing area
  Vout& main() { return area(AreaIndex::Main).out; }
  Vout& cold() { return area(AreaIndex::Cold).out; }
  Vout& frozen() { return area(AreaIndex::Frozen).out; }

  // create areas
  Vout& main(CodeBlock& cb) { return add(cb, AreaIndex::Main); }
  Vout& cold(CodeBlock& cb) { return add(cb, AreaIndex::Cold); }
  Vout& frozen(CodeBlock& cb) { return add(cb, AreaIndex::Frozen); }
  Vout& main(X64Assembler& a) { return main(a.code()); }
  Vout& cold(X64Assembler& a) { return cold(a.code()); }
  Vout& frozen(X64Assembler& a) { return frozen(a.code()); }
  Vunit& unit() { return m_unit; }
  jit::vector<Area>& areas() { return m_areas; }

private:
  Vout& add(CodeBlock &cb, AreaIndex area);
  Area& area(AreaIndex i) {
    assert((unsigned)i < m_areas.size());
    return m_areas[(unsigned)i];
  }

private:
  Vmeta* const m_meta;
  Vunit m_unit;
  jit::vector<Area> m_areas; // indexed by AreaIndex
};

/*
 * Vauto is a convenience helper for emitting small amounts of machine code
 * using vasm. It always has a main code block; cold and frozen blocks may be
 * added using the normal Vasm API after creation. When the Vauto goes out of
 * scope, it will finalize and emit any code it contains.
 */
struct Vauto : Vasm {
  explicit Vauto(CodeBlock& code)
    : Vasm(nullptr)
  {
    unit().entry = Vlabel(main(code));
  }
  ~Vauto();
};

template<class F> void visit(const Vunit&, Vreg v, F f) {
  f(v);
}
template<class F> void visit(const Vunit&, Vptr p, F f) {
  if (p.base.isValid()) f(p.base);
  if (p.index.isValid()) f(p.index);
}
template<class F> void visit(const Vunit& unit, Vtuple t, F f) {
  for (auto r : unit.tuples[t]) f(r);
}
template<class F> void visit(const Vunit& unit, VcallArgsId a, F f) {
  auto& args = unit.vcallArgs[a];
  for (auto r : args.args) f(r);
  for (auto r : args.simdArgs) f(r);
  for (auto r : args.stkArgs) f(r);
}
template<class F> void visit(const Vunit& unit, RegSet regs, F f) {
  regs.forEach([&](Vreg r) { f(r); });
}

template<class Use>
void visitUses(const Vunit& unit, Vinstr& inst, Use use) {
  switch (inst.op) {
#define O(name, imms, uses, defs) \
    case Vinstr::name: { \
      auto& i = inst.name##_; (void)i; \
      uses \
      break; \
    }
#define U(s) visit(unit, i.s, use);
#define UA(s) visit(unit, i.s, use);
#define UH(s,h) visit(unit, i.s, use);
#define Un
    X64_OPCODES
#undef Un
#undef UH
#undef UA
#undef U
#undef O
  }
}

template<class Def>
void visitDefs(const Vunit& unit, const Vinstr& inst, Def def) {
  switch (inst.op) {
#define O(name, imms, uses, defs) \
    case Vinstr::name: { \
      auto& i = inst.name##_; (void)i; \
      defs \
      break; \
    }
#define D(d) visit(unit, i.d, def);
#define DH(d,h) visit(unit, i.d, def);
#define Dn
    X64_OPCODES
#undef Dn
#undef DH
#undef D
#undef O
  }
}

/*
 * visitOperands visits all operands of the given instruction, calling
 * visitor.imm(), visitor.use(), visitor.across(), and visitor.def() as defined
 * in the X64_OPCODES macro.
 *
 * The template spew is necessary to support callers that only have a const
 * Vinstr& as well as callers with a Vinstr& that wish to mutate the
 * instruction in the visitor.
 */
template<class MaybeConstVinstr, class Visitor>
typename std::enable_if<
  std::is_same<MaybeConstVinstr, Vinstr>::value ||
  std::is_same<MaybeConstVinstr, const Vinstr>::value
>::type
visitOperands(MaybeConstVinstr& inst, Visitor& visitor) {
  switch (inst.op) {
#define O(name, imms, uses, defs) \
    case Vinstr::name: { \
      auto& i = inst.name##_; (void)i; \
      imms \
      uses \
      defs \
      break; \
    }
#define I(f) visitor.imm(i.f);
#define U(s) visitor.use(i.s);
#define UA(s) visitor.across(i.s);
#define UH(s,h) visitor.useHint(i.s, i.h);
#define D(d) visitor.def(i.d);
#define DH(d,h) visitor.defHint(i.d, i.h);
#define Inone
#define Un
#define Dn
    X64_OPCODES
#undef Dn
#undef Un
#undef Inone
#undef DH
#undef D
#undef UH
#undef UA
#undef U
#undef I
#undef O
  }
}

// visit reachable blocks in postorder, calling fn on each one.
struct PostorderWalker {
  template<class Fn> void dfs(Vlabel b, Fn fn) {
    if (visited.test(b)) return;
    visited.set(b);
    for (auto s : succs(unit.blocks[b])) {
      dfs(s, fn);
    }
    fn(b);
  }
  template<class Fn> void dfs(Fn fn) {
    dfs(unit.entry, fn);
  }
  explicit PostorderWalker(const Vunit& u)
    : unit(u)
    , visited(u.blocks.size())
  {}
  const Vunit& unit;
  boost::dynamic_bitset<> visited;
};

extern const char* vinst_names[];
bool isBlockEnd(Vinstr& inst);
std::string format(Vreg);
bool check(Vunit&);
bool checkBlockEnd(Vunit& v, Vlabel b);

// search for the phidef in block b, then return its dest tuple
Vtuple findDefs(const Vunit& unit, Vlabel b);

typedef jit::vector<jit::vector<Vlabel>> PredVector;
PredVector computePreds(const Vunit& unit);

}}
#endif
