// Copyright 2022 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/builtins/builtins-utils-gen.h"
#include "src/builtins/builtins.h"
#include "src/codegen/code-stub-assembler.h"

namespace v8 {
namespace internal {

class ShadowRealmBuiltinsAssembler : public CodeStubAssembler {
 public:
  explicit ShadowRealmBuiltinsAssembler(compiler::CodeAssemblerState* state)
      : CodeStubAssembler(state) {}

 protected:
  TNode<JSObject> AllocateJSWrappedFunction(TNode<Context> context);
};

TNode<JSObject> ShadowRealmBuiltinsAssembler::AllocateJSWrappedFunction(
    TNode<Context> context) {
  TNode<NativeContext> native_context = LoadNativeContext(context);
  TNode<Map> map = CAST(
      LoadContextElement(native_context, Context::WRAPPED_FUNCTION_MAP_INDEX));
  return AllocateJSObjectFromMap(map);
}

// https://tc39.es/proposal-shadowrealm/#sec-getwrappedvalue
TF_BUILTIN(ShadowRealmGetWrappedValue, ShadowRealmBuiltinsAssembler) {
  auto context = Parameter<Context>(Descriptor::kContext);
  auto creation_context = Parameter<Context>(Descriptor::kCreationContext);
  auto value = Parameter<Object>(Descriptor::kValue);

  Label if_primitive(this), if_callable(this), unwrap(this), wrap(this),
      bailout(this, Label::kDeferred);

  // 2. Return value.
  GotoIf(TaggedIsSmi(value), &if_primitive);
  GotoIfNot(IsJSReceiver(CAST(value)), &if_primitive);

  // 1. If Type(value) is Object, then
  // 1a. If IsCallable(value) is false, throw a TypeError exception.
  // 1b. Return ? WrappedFunctionCreate(callerRealm, value).
  Branch(IsCallable(CAST(value)), &if_callable, &bailout);

  BIND(&if_primitive);
  Return(value);

  BIND(&if_callable);
  TVARIABLE(Object, target);
  target = value;
  // WrappedFunctionCreate
  // https://tc39.es/proposal-shadowrealm/#sec-wrappedfunctioncreate
  Branch(IsJSWrappedFunction(CAST(value)), &unwrap, &wrap);

  BIND(&unwrap);
  // The intermediate wrapped functions are not user-visible. And calling a
  // wrapped function won't cause a side effect in the creation realm.
  // Unwrap here to avoid nested unwrapping at the call site.
  TNode<JSWrappedFunction> target_wrapped_function = CAST(value);
  target = LoadObjectField(target_wrapped_function,
                           JSWrappedFunction::kWrappedTargetFunctionOffset);
  Goto(&wrap);

  BIND(&wrap);
  // 1. Let internalSlotsList be the internal slots listed in Table 2, plus
  // [[Prototype]] and [[Extensible]].
  // 2. Let wrapped be ! MakeBasicObject(internalSlotsList).
  // 3. Set wrapped.[[Prototype]] to
  // callerRealm.[[Intrinsics]].[[%Function.prototype%]].
  // 4. Set wrapped.[[Call]] as described in 2.1.
  TNode<JSObject> wrapped = AllocateJSWrappedFunction(creation_context);

  // 5. Set wrapped.[[WrappedTargetFunction]] to Target.
  StoreObjectFieldNoWriteBarrier(
      wrapped, JSWrappedFunction::kWrappedTargetFunctionOffset, target.value());
  // 6. Set wrapped.[[Realm]] to callerRealm.
  StoreObjectFieldNoWriteBarrier(wrapped, JSWrappedFunction::kContextOffset,
                                 creation_context);

  // 7. Let result be CopyNameAndLength(wrapped, Target, "wrapped").
  // 8. If result is an Abrupt Completion, throw a TypeError exception.
  // TODO(v8:11989): https://github.com/tc39/proposal-shadowrealm/pull/348

  // 9. Return wrapped.
  Return(wrapped);

  BIND(&bailout);
  ThrowTypeError(context, MessageTemplate::kNotCallable, value);
}

// https://tc39.es/proposal-shadowrealm/#sec-wrapped-function-exotic-objects-call-thisargument-argumentslist
TF_BUILTIN(CallWrappedFunction, ShadowRealmBuiltinsAssembler) {
  auto argc = UncheckedParameter<Int32T>(Descriptor::kActualArgumentsCount);
  TNode<IntPtrT> argc_ptr = ChangeInt32ToIntPtr(argc);
  auto wrapped_function = Parameter<JSWrappedFunction>(Descriptor::kFunction);
  auto context = Parameter<Context>(Descriptor::kContext);

  PerformStackCheck(context);

  Label call_exception(this, Label::kDeferred),
      target_not_callable(this, Label::kDeferred);

  // 1. Let target be F.[[WrappedTargetFunction]].
  TNode<JSReceiver> target = CAST(LoadObjectField(
      wrapped_function, JSWrappedFunction::kWrappedTargetFunctionOffset));
  // 2. Assert: IsCallable(target) is true.
  CSA_DCHECK(this, IsCallable(target));

  // 4. Let callerRealm be ? GetFunctionRealm(F).
  TNode<Context> caller_context = LoadObjectField<Context>(
      wrapped_function, JSWrappedFunction::kContextOffset);
  // 3. Let targetRealm be ? GetFunctionRealm(target).
  TNode<Context> target_context =
      GetFunctionRealm(caller_context, target, &target_not_callable);
  // 5. NOTE: Any exception objects produced after this point are associated
  // with callerRealm.

  CodeStubArguments args(this, argc_ptr);
  TNode<Object> receiver = args.GetReceiver();

  // 6. Let wrappedArgs be a new empty List.
  TNode<FixedArray> wrapped_args =
      CAST(AllocateFixedArray(ElementsKind::PACKED_ELEMENTS, argc_ptr));
  // Fill the fixed array so that heap verifier doesn't complain about it.
  FillFixedArrayWithValue(ElementsKind::PACKED_ELEMENTS, wrapped_args,
                          IntPtrConstant(0), argc_ptr,
                          RootIndex::kUndefinedValue);

  // 8. Let wrappedThisArgument to ? GetWrappedValue(targetRealm, thisArgument).
  // Create wrapped value in the target realm.
  TNode<Object> wrapped_receiver =
      CallBuiltin(Builtin::kShadowRealmGetWrappedValue, caller_context,
                  target_context, receiver);
  StoreFixedArrayElement(wrapped_args, 0, wrapped_receiver);
  // 7. For each element arg of argumentsList, do
  BuildFastLoop<IntPtrT>(
      IntPtrConstant(0), args.GetLengthWithoutReceiver(),
      [&](TNode<IntPtrT> index) {
        // 7a. Let wrappedValue be ? GetWrappedValue(targetRealm, arg).
        // Create wrapped value in the target realm.
        TNode<Object> wrapped_value =
            CallBuiltin(Builtin::kShadowRealmGetWrappedValue, caller_context,
                        target_context, args.AtIndex(index));
        // 7b. Append wrappedValue to wrappedArgs.
        StoreFixedArrayElement(
            wrapped_args, IntPtrAdd(index, IntPtrConstant(1)), wrapped_value);
      },
      1, IndexAdvanceMode::kPost);

  TVARIABLE(Object, var_exception);
  TNode<Object> result;
  {
    compiler::ScopedExceptionHandler handler(this, &call_exception,
                                             &var_exception);
    TNode<Int32T> args_count = Int32Constant(0);  // args already on the stack
    Callable callable = CodeFactory::CallVarargs(isolate());

    // 9. Let result be the Completion Record of Call(target,
    // wrappedThisArgument, wrappedArgs).
    result = CallStub(callable, target_context, target, args_count, argc,
                      wrapped_args);
  }

  // 10. If result.[[Type]] is normal or result.[[Type]] is return, then
  // 10a. Return ? GetWrappedValue(callerRealm, result.[[Value]]).
  TNode<Object> wrapped_result =
      CallBuiltin(Builtin::kShadowRealmGetWrappedValue, caller_context,
                  caller_context, result);
  args.PopAndReturn(wrapped_result);

  // 11. Else,
  BIND(&call_exception);
  // 11a. Throw a TypeError exception.
  // TODO(v8:11989): provide a non-observable inspection.
  ThrowTypeError(context, MessageTemplate::kCallShadowRealmFunctionThrown,
                 var_exception.value());

  BIND(&target_not_callable);
  // A wrapped value should not be non-callable.
  Unreachable();
}

}  // namespace internal
}  // namespace v8