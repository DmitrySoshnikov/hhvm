(**
 * Copyright (c) 2014, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the "hack" directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 *
 *)

(*****************************************************************************)
(* Module used to enforce that Enum subclasses are used reasonably.
 * Exports the Enum type as the type of all constants, checks that constants
 * have the proper type, and restricts what types can be used for enums.
 *)
(*****************************************************************************)
open Nast
open Utils
open Typing_defs

(* Figures out if a class needs to be treated like an enum and if so returns
 * Some(base, type, constraint), where base is the underlying type of the
 * enum, type is the actual type of enum elements, and constraint is
 * an optional subtyping constraint. For subclasses of Enum<T>, both
 * base and type these are T.
 * For first-class enums, we distinguish between these. *)
let is_enum name enum ancestors =
  match enum with
    | None ->
      (match SMap.get "\\Enum" ancestors with
        | Some (_, (Tapply ((_, "\\Enum"), [ty_exp]))) ->
          (* If the class is a subclass of UncheckedEnum, ignore it. *)
          if SMap.mem "\\UncheckedEnum" ancestors then None
          else Some (ty_exp, ty_exp, None)
        | _ -> None)
    | Some enum ->
      Some (enum.te_base, (fst enum.te_base, Tapply (name, [])),
            enum.te_constraint)

(* Check that a type is something that can be used as an array index
 * (int or string), blowing through typedefs to do it. Takes a function
 * to call to report the error if it isn't. *)
let check_valid_array_key_type f_fail ~allow_any:allow_any env p t =
  let env, (r, t'), trail = Typing_tdef.force_expand_typedef env t in
  (match t' with
    | Tprim Tint | Tprim Tstring -> ()
    (* Enums have to be valid array keys *)
    | Tapply ((_, x), _) when Typing_env.is_enum env x -> ()
    | Tany when allow_any -> ()
    | _ -> f_fail p (Reason.to_pos r) (Typing_print.error t') trail);
  env

let enum_check_const ty_exp env (_, (p, _), _) t =
  (* Constants need to be subtypes of the enum type *)
  let env = Typing_ops.sub_type p Reason.URenum env ty_exp t in
  (* Make sure the underlying type of the constant is an int
   * or a string. This matters because we need to only allow
   * int and string constants (since only they can be array
   * indexes). *)
  (* Need to allow Tany, since we might not have the types *)
  check_valid_array_key_type
    Errors.enum_constant_type_bad
    ~allow_any:true env p t

(* If a class is a subclass of Enum<T>, check that the types of all of
 * the constants are compatible with T.
 * Also make sure that T is either int, string, or mixed (or an
 * abstract type that is one of those under the hood), that all
 * constants are ints or strings when T is mixed, and that any type
 * hints are compatible with the type. *)
let enum_class_check env tc consts const_types =
  match is_enum (tc.tc_pos, tc.tc_name) tc.tc_enum_type tc.tc_ancestors with
    | Some (ty_exp, _, ty_constraint) ->
        let env, (r, ty_exp'), trail =
          Typing_tdef.force_expand_typedef env ty_exp in
        (match ty_exp' with
          (* We disallow typedefs that point to mixed *)
          | Tmixed -> if snd ty_exp <> Tmixed then
              Errors.enum_type_typedef_mixed (Reason.to_pos r)
          | Tprim Tint | Tprim Tstring -> ()
          (* Allow enums in terms of other enums *)
          | Tapply ((_, x), _) when Typing_env.is_enum env x -> ()
          (* Don't tell anyone, but we allow type params too, since there are
           * Enum subclasses that need to do that *)
          | Tgeneric _ -> ()
          | _ -> Errors.enum_type_bad (Reason.to_pos r)
                   (Typing_print.error ty_exp') trail);

        (* Make sure that if a constraint was given that the base type is
         * actually a subtype of it. *)
        let env = (match ty_constraint with
          | Some ty ->
            Typing_ops.sub_type tc.tc_pos Reason.URenum_cstr env ty ty_exp
          | None -> env) in

        List.fold_left2 (enum_check_const ty_exp) env consts const_types

    | None -> env

(* If a class is an Enum, we give all of the constants in the class
 * the type of the Enum. We don't do this for Enum<mixed>, since that
 * could *lose* type information.
 *)
let enum_class_decl_rewrite env name enum ancestors consts =
  if Typing_env.is_decl env then consts else
    match is_enum name enum ancestors with
      | None
      | Some (_, (_, Tmixed), _) -> consts
      | Some (_, ty, _) ->
      (* A special constant called "class" gets added, and we don't
       * want to rewrite its type. *)
      SMap.mapi (function k -> function c ->
                 if k = "class" then c else {c with ce_type = ty})
        consts

let get_constant tc (seen, has_default) = function
  | Default _ -> (seen, true)
  | Case ((pos, Class_const (CI (_, cls), (_, const))), _) ->
    if cls <> tc.tc_name then
      (Errors.enum_switch_wrong_class pos (strip_ns tc.tc_name) (strip_ns cls);
       (seen, has_default))
    else
      (match SMap.get const seen with
        | None -> (SMap.add const pos seen, has_default)
        | Some old_pos ->
          Errors.enum_switch_redundant const old_pos pos;
          (seen, has_default))
  | Case ((pos, _), _) ->
    Errors.enum_switch_not_const pos;
    (seen, has_default)

let check_enum_exhaustiveness env pos tc caselist =
  let (seen, has_default) =
    List.fold_left (get_constant tc) (SMap.empty, false) caselist in
  let consts = SMap.remove "class" tc.tc_consts in
  let all_cases_handled = SMap.cardinal seen = SMap.cardinal consts in
  match (all_cases_handled, has_default) with
    | false, false ->
      let const_list = SMap.keys consts in
      let unhandled =
        List.filter (function k -> not (SMap.mem k seen)) const_list in
      Errors.enum_switch_nonexhaustive pos unhandled tc.tc_pos
    | true, true -> Errors.enum_switch_redundant_default pos tc.tc_pos
    | _ -> ()
