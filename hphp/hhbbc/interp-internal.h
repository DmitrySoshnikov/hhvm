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
#ifndef incl_HPHP_INTERP_INTERNAL_H_
#define incl_HPHP_INTERP_INTERNAL_H_

#include <algorithm>

#include <folly/Optional.h>

#include "hphp/runtime/base/type-string.h"

#include "hphp/hhbbc/interp-state.h"
#include "hphp/hhbbc/interp.h"
#include "hphp/hhbbc/representation.h"
#include "hphp/hhbbc/type-system.h"
#include "hphp/hhbbc/func-util.h"

namespace HPHP { namespace HHBBC {

//////////////////////////////////////////////////////////////////////

TRACE_SET_MOD(hhbbc);

const StaticString s_extract("extract");
const StaticString s_extract_sl("__SystemLib\\extract");
const StaticString s_parse_str("parse_str");
const StaticString s_parse_str_sl("__SystemLib\\parse_str");
const StaticString s_compact("compact");
const StaticString s_compact_sl("__SystemLib\\compact_sl");
const StaticString s_get_defined_vars("get_defined_vars");
const StaticString s_get_defined_vars_sl("__SystemLib\\get_defined_vars");

const StaticString s_http_response_header("http_response_header");
const StaticString s_php_errormsg("php_errormsg");

//////////////////////////////////////////////////////////////////////

/*
 * Interpreter Step State.
 *
 * This struct gives interpreter functions access to shared state.  It's not in
 * interp-state.h because it's part of the internal implementation of
 * interpreter routines.  The publicized state as results of interpretation are
 * in that header and interp.h.
 */
struct ISS {
  explicit ISS(Interp& bag,
               StepFlags& flags,
               PropagateFn propagate)
    : index(bag.index)
    , ctx(bag.ctx)
    , collect(bag.collect)
    , blk(*bag.blk)
    , state(bag.state)
    , flags(flags)
    , propagate(propagate)
  {}

  const Index& index;
  const Context ctx;
  CollectedInfo& collect;
  const php::Block& blk;
  State& state;
  StepFlags& flags;
  PropagateFn propagate;
};

//////////////////////////////////////////////////////////////////////

namespace {

void nothrow(ISS& env) {
  FTRACE(2, "    nothrow\n");
  env.flags.wasPEI = false;
}

void unreachable(ISS& env)    { env.state.unreachable = true; }
void constprop(ISS& env)      { env.flags.canConstProp = true; }

void jmp_nofallthrough(ISS& env) {
  env.flags.jmpFlag = StepFlags::JmpFlags::Taken;
}
void jmp_nevertaken(ISS& env) {
  env.flags.jmpFlag = StepFlags::JmpFlags::Fallthrough;
}

void readUnknownLocals(ISS& env) { env.flags.mayReadLocalSet.set(); }
void readAllLocals(ISS& env)     { env.flags.mayReadLocalSet.set(); }

void killLocals(ISS& env) {
  FTRACE(2, "    killLocals\n");
  readUnknownLocals(env);
  for (auto& l : env.state.locals) l = TGen;
}

void doRet(ISS& env, Type t) {
  readAllLocals(env);
  assert(env.state.stack.empty());
  env.flags.returned = t;
}

void specialFunctionEffects(ISS& env, SString name) {
  auto special_fn = [&] (const StaticString& fn1, const StaticString& fn2) {
    return name->isame(fn1.get()) ||
      (!options.DisallowDynamicVarEnvFuncs && name->isame(fn2.get()));
  };

  // extract() and parse_str() can trash the local variable environment.
  if (special_fn(s_extract_sl, s_extract) ||
      special_fn(s_parse_str_sl, s_parse_str)) {
    readUnknownLocals(env);
    killLocals(env);
    return;
  }
  // compact() and get_defined_vars() read the local variable
  // environment.  We could check which locals for compact, but for
  // now we just include them all.
  if (special_fn(s_get_defined_vars_sl, s_get_defined_vars) ||
      special_fn(s_compact_sl, s_compact)) {
    readUnknownLocals(env);
    return;
  }
}

void specialFunctionEffects(ISS& env, ActRec ar) {
  switch (ar.kind) {
  case FPIKind::Unknown:
    // fallthrough
  case FPIKind::Func:
    if (!ar.func) {
      if (!options.DisallowDynamicVarEnvFuncs) {
        readUnknownLocals(env);
        killLocals(env);
      }
      return;
    }
    specialFunctionEffects(env, ar.func->name());
    break;
  case FPIKind::Ctor:
  case FPIKind::ObjMeth:
  case FPIKind::ClsMeth:
  case FPIKind::ObjInvoke:
  case FPIKind::CallableArr:
    break;
  }
}

//////////////////////////////////////////////////////////////////////
// eval stack

Type popT(ISS& env) {
  assert(!env.state.stack.empty());
  auto const ret = env.state.stack.back();
  FTRACE(2, "    pop:  {}\n", show(ret));
  env.state.stack.pop_back();
  return ret;
}

Type popC(ISS& env) {
  auto const v = popT(env);
  assert(v.subtypeOf(TInitCell)); // or it would be popU, which doesn't exist
  return v;
}

Type popV(ISS& env) {
  auto const v = popT(env);
  assert(v.subtypeOf(TRef));
  return v;
}

Type popA(ISS& env) {
  auto const v = popT(env);
  assert(v.subtypeOf(TCls));
  return v;
}

Type popR(ISS& env)  { return popT(env); }
Type popF(ISS& env)  { return popT(env); }
Type popCV(ISS& env) { return popT(env); }
Type popU(ISS& env)  { return popT(env); }

void popFlav(ISS& env, Flavor flav) {
  switch (flav) {
  case Flavor::C: popC(env); break;
  case Flavor::V: popV(env); break;
  case Flavor::U: popU(env); break;
  case Flavor::F: popF(env); break;
  case Flavor::R: popR(env); break;
  case Flavor::A: popA(env); break;
  }
}

Type topT(ISS& env, uint32_t idx = 0) {
  assert(idx < env.state.stack.size());
  return env.state.stack[env.state.stack.size() - idx - 1];
}

Type topA(ISS& env, uint32_t i = 0) {
  assert(topT(env, i).subtypeOf(TCls));
  return topT(env, i);
}

Type topC(ISS& env, uint32_t i = 0) {
  assert(topT(env, i).subtypeOf(TInitCell));
  return topT(env, i);
}

Type topR(ISS& env, uint32_t i = 0) { return topT(env, i); }

Type topV(ISS& env, uint32_t i = 0) {
  assert(topT(env, i).subtypeOf(TRef));
  return topT(env, i);
}

void push(ISS& env, Type t) {
  FTRACE(2, "    push: {}\n", show(t));
  env.state.stack.push_back(t);
}

//////////////////////////////////////////////////////////////////////
// fpi

void fpiPush(ISS& env, ActRec ar) {
  FTRACE(2, "    fpi+: {}\n", show(ar));
  env.state.fpiStack.push_back(ar);
}

ActRec fpiPop(ISS& env) {
  assert(!env.state.fpiStack.empty());
  auto const ret = env.state.fpiStack.back();
  FTRACE(2, "    fpi-: {}\n", show(ret));
  env.state.fpiStack.pop_back();
  return ret;
}

ActRec fpiTop(ISS& env) {
  assert(!env.state.fpiStack.empty());
  return env.state.fpiStack.back();
}

PrepKind prepKind(ISS& env, uint32_t paramId) {
  auto ar = fpiTop(env);
  if (ar.func) return env.index.lookup_param_prep(env.ctx, *ar.func, paramId);
  return PrepKind::Unknown;
}

//////////////////////////////////////////////////////////////////////
// locals

/*
 * Locals with certain special names can be set in the enclosing scope by
 * various php routines.  We don't attempt to track their types.  Furthermore,
 * in a pseudomain effectively all 'locals' are volatile, because any re-entry
 * could modify them through $GLOBALS, so in a pseudomain we don't track any
 * local types.
 */
bool isVolatileLocal(ISS& env, borrowed_ptr<const php::Local> l) {
  if (is_pseudomain(env.ctx.func)) return true;
  // Note: unnamed locals in a pseudomain probably are safe (i.e. can't be
  // changed through $GLOBALS), but for now we don't bother.
  if (!l->name) return false;
  return l->name->same(s_http_response_header.get()) ||
         l->name->same(s_php_errormsg.get());
}

void mayReadLocal(ISS& env, uint32_t id) {
  if (id < env.flags.mayReadLocalSet.size()) {
    env.flags.mayReadLocalSet.set(id);
  }
}

Type locRaw(ISS& env, borrowed_ptr<const php::Local> l) {
  mayReadLocal(env, l->id);
  auto ret = env.state.locals[l->id];
  if (isVolatileLocal(env, l)) {
    always_assert_flog(ret == TGen, "volatile local was not TGen");
  }
  return ret;
}

void setLocRaw(ISS& env, borrowed_ptr<const php::Local> l, Type t) {
  mayReadLocal(env, l->id);
  if (isVolatileLocal(env, l)) {
    auto current = env.state.locals[l->id];
    always_assert_flog(current == TGen, "volatile local was not TGen");
    return;
  }
  env.state.locals[l->id] = t;
}

// Read a local type in the sense of CGetL.  (TUninits turn into
// TInitNull, and potentially reffy types return the "inner" type,
// which is always a subtype of InitCell.)
Type locAsCell(ISS& env, borrowed_ptr<const php::Local> l) {
  auto t = locRaw(env, l);
  return !t.subtypeOf(TCell) ? TInitCell :
          t.subtypeOf(TUninit) ? TInitNull :
          remove_uninit(t);
}

// Read a local type, dereferencing refs, but without converting
// potential TUninits to TInitNull.
Type derefLoc(ISS& env, borrowed_ptr<const php::Local> l) {
  auto v = locRaw(env, l);
  if (v.subtypeOf(TCell)) return v;
  return v.couldBe(TUninit) ? TCell : TInitCell;
}

void ensureInit(ISS& env, borrowed_ptr<const php::Local> l) {
  auto t = locRaw(env, l);
  if (isVolatileLocal(env, l)) {
    always_assert_flog(t == TGen, "volatile local was not TGen");
    return;
  }
  if (t.couldBe(TUninit)) {
    if (t.subtypeOf(TUninit)) return setLocRaw(env, l, TInitNull);
    if (t.subtypeOf(TCell))   return setLocRaw(env, l, remove_uninit(t));
    setLocRaw(env, l, TInitGen);
  }
}

bool locCouldBeUninit(ISS& env, borrowed_ptr<const php::Local> l) {
  return locRaw(env, l).couldBe(TUninit);
}

/*
 * Set a local type in the sense of tvSet.  If the local is boxed or
 * not known to be not boxed, we can't change the type.  May be used
 * to set locals to types that include Uninit.
 */
void setLoc(ISS& env, borrowed_ptr<const php::Local> l, Type t) {
  auto v = locRaw(env, l);
  if (isVolatileLocal(env, l)) {
    always_assert_flog(v == TGen, "volatile local was not TGen");
    return;
  }
  if (v.subtypeOf(TCell)) env.state.locals[l->id] = t;
}

borrowed_ptr<php::Local> findLocal(ISS& env, SString name) {
  for (auto& l : env.ctx.func->locals) {
    if (l->name->same(name)) {
      mayReadLocal(env, l->id);
      return borrow(l);
    }
  }
  return nullptr;
}

// Force non-ref locals to TCell.  Used when something modifies an
// unknown local's value, without changing reffiness.
void loseNonRefLocalTypes(ISS& env) {
  readUnknownLocals(env);
  FTRACE(2, "    loseNonRefLocalTypes\n");
  for (auto& l : env.state.locals) {
    if (l.subtypeOf(TCell)) l = TCell;
  }
}

void boxUnknownLocal(ISS& env) {
  readUnknownLocals(env);
  FTRACE(2, "   boxUnknownLocal\n");
  for (auto& l : env.state.locals) {
    if (!l.subtypeOf(TRef)) l = TGen;
  }
}

void unsetUnknownLocal(ISS& env) {
  readUnknownLocals(env);
  FTRACE(2, "  unsetUnknownLocal\n");
  for (auto& l : env.state.locals) l = union_of(l, TUninit);
}

//////////////////////////////////////////////////////////////////////
// iterators

void setIter(ISS& env, borrowed_ptr<php::Iter> iter, Iter iterState) {
  env.state.iters[iter->id] = std::move(iterState);
}
void freeIter(ISS& env, borrowed_ptr<php::Iter> iter) {
  env.state.iters[iter->id] = UnknownIter {};
}

//////////////////////////////////////////////////////////////////////
// $this

void setThisAvailable(ISS& env) {
  FTRACE(2, "    setThisAvailable\n");
  env.state.thisAvailable = true;
}

bool thisAvailable(ISS& env) { return env.state.thisAvailable; }

// Returns the type $this would have if it's not null.  Generally
// you have to check thisIsAvailable() before assuming it can't be
// null.
folly::Optional<Type> thisType(ISS& env) {
  if (!env.ctx.cls) return folly::none;
  if (auto const rcls = env.index.resolve_class(env.ctx, env.ctx.cls->name)) {
    return subObj(*rcls);
  }
  return folly::none;
}

folly::Optional<Type> selfCls(ISS& env) {
  if (!env.ctx.cls) return folly::none;
  if (auto const rcls = env.index.resolve_class(env.ctx, env.ctx.cls->name)) {
    return subCls(*rcls);
  }
  return folly::none;
}

folly::Optional<Type> selfClsExact(ISS& env) {
  if (!env.ctx.cls) return folly::none;
  if (auto const rcls = env.index.resolve_class(env.ctx, env.ctx.cls->name)) {
    return clsExact(*rcls);
  }
  return folly::none;
}

//////////////////////////////////////////////////////////////////////
// properties on $this

/*
 * Note: we are only tracking control-flow insensitive types for
 * object properties, because it can be pretty rough to try to track
 * all cases that could re-enter the VM, run arbitrary code, and
 * potentially change the type of a property.
 *
 * Because of this, the various "setter" functions for thisProps
 * here actually just union the new type into what we already had.
 */

Type* thisPropRaw(ISS& env, SString name) {
  auto& privateProperties = env.collect.props.privateProperties();
  auto const it = privateProperties.find(name);
  if (it != end(privateProperties)) {
    return &it->second;
  }
  return nullptr;
}

bool isTrackedThisProp(ISS& env, SString name) {
  return thisPropRaw(env, name);
}

void killThisProps(ISS& env) {
  FTRACE(2, "    killThisProps\n");
  for (auto& kv : env.collect.props.privateProperties()) {
    kv.second = TGen;
  }
}

/*
 * This function returns a type that includes all the possible types
 * that could result from reading a property $this->name.
 *
 * Note that this may include types that the property itself cannot
 * actually contain, due to the effects of a possible __get function.
 */
folly::Optional<Type> thisPropAsCell(ISS& env, SString name) {
  auto const t = thisPropRaw(env, name);
  if (!t) return folly::none;
  if (t->couldBe(TUninit)) {
    auto const rthis = thisType(env);
    if (!rthis || dobj_of(*rthis).cls.couldHaveMagicGet()) {
      return TInitCell;
    }
  }
  return !t->subtypeOf(TCell) ? TInitCell :
          t->subtypeOf(TUninit) ? TInitNull :
          remove_uninit(*t);
}

/*
 * Merge a type into the track property types on $this, in the sense
 * of tvSet (i.e. setting the inner type on possible refs).
 *
 * Note that all types we see that could go into an object property
 * have to loosen_statics and loosen_values.  This is because the
 * object could be serialized and then deserialized, losing the
 * static-ness of a string or array member, and we don't guarantee
 * deserialization would preserve a constant value object property
 * type.
 */
void mergeThisProp(ISS& env, SString name, Type type) {
  auto const t = thisPropRaw(env, name);
  if (!t) return;
  *t = union_of(*t, loosen_statics(loosen_values(type)));
}

/*
 * Merge something into each this prop.  Usually MapFn will be a
 * predicate that returns TBottom when some condition doesn't hold.
 *
 * The types given to the map function are the raw tracked types
 * (i.e. could be TRef or TUninit).
 */
template<class MapFn>
void mergeEachThisPropRaw(ISS& env, MapFn fn) {
  for (auto& kv : env.collect.props.privateProperties()) {
    mergeThisProp(env, kv.first, fn(kv.second));
  }
}

void unsetThisProp(ISS& env, SString name) {
  mergeThisProp(env, name, TUninit);
}

void unsetUnknownThisProp(ISS& env) {
  for (auto& kv : env.collect.props.privateProperties()) {
    mergeThisProp(env, kv.first, TUninit);
  }
}

void boxThisProp(ISS& env, SString name) {
  auto const t = thisPropRaw(env, name);
  if (!t) return;
  *t = union_of(*t, TRef);
}

/*
 * Forces non-ref property types up to TCell.  This is used when an
 * operation affects an unknown property on $this, but can't change
 * its reffiness.  This could only do TInitCell, but we're just
 * going to gradually get rid of the callsites of this.
 */
void loseNonRefThisPropTypes(ISS& env) {
  FTRACE(2, "    loseNonRefThisPropTypes\n");
  for (auto& kv : env.collect.props.privateProperties()) {
    if (kv.second.subtypeOf(TCell)) kv.second = TCell;
  }
}

//////////////////////////////////////////////////////////////////////
// properties on self::

// Similar to $this properties above, we only track control-flow
// insensitive types for these.

Type* selfPropRaw(ISS& env, SString name) {
  auto& privateStatics = env.collect.props.privateStatics();
  auto it = privateStatics.find(name);
  if (it != end(privateStatics)) {
    return &it->second;
  }
  return nullptr;
}

void killSelfProps(ISS& env) {
  FTRACE(2, "    killSelfProps\n");
  for (auto& kv : env.collect.props.privateStatics()) {
    kv.second = TGen;
  }
}

void killSelfProp(ISS& env, SString name) {
  FTRACE(2, "    killSelfProp {}\n", name->data());
  if (auto t = selfPropRaw(env, name)) *t = TGen;
}

// TODO(#3684136): self::$foo can't actually ever be uninit.  Right
// now uninits may find their way into here though.
folly::Optional<Type> selfPropAsCell(ISS& env, SString name) {
  auto const t = selfPropRaw(env, name);
  if (!t) return folly::none;
  return !t->subtypeOf(TCell) ? TInitCell :
          t->subtypeOf(TUninit) ? TInitNull :
          remove_uninit(*t);
}

/*
 * Merges a type into tracked static properties on self, in the
 * sense of tvSet (i.e. setting the inner type on possible refs).
 */
void mergeSelfProp(ISS& env, SString name, Type type) {
  auto const t = selfPropRaw(env, name);
  if (!t) return;
  *t = union_of(*t, type);
}

/*
 * Similar to mergeEachThisPropRaw, but for self props.
 */
template<class MapFn>
void mergeEachSelfPropRaw(ISS& env, MapFn fn) {
  for (auto& kv : env.collect.props.privateStatics()) {
    mergeSelfProp(env, kv.first, fn(kv.second));
  }
}

void boxSelfProp(ISS& env, SString name) {
  mergeSelfProp(env, name, TRef);
}

/*
 * Forces non-ref static properties up to TCell.  This is used when
 * an operation affects an unknown static property on self::, but
 * can't change its reffiness.
 *
 * This could only do TInitCell because static properties can never
 * be unset.  We're just going to get rid of the callers of this
 * function over a few more changes, though.
 */
void loseNonRefSelfPropTypes(ISS& env) {
  FTRACE(2, "    loseNonRefSelfPropTypes\n");
  for (auto& kv : env.collect.props.privateStatics()) {
    if (kv.second.subtypeOf(TInitCell)) kv.second = TCell;
  }
}

}

//////////////////////////////////////////////////////////////////////

}}

#endif
