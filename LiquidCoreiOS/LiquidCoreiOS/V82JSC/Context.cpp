//
//  Context.cpp
//  LiquidCore
//
//  Created by Eric Lange on 1/28/18.
//  Copyright © 2018 LiquidPlayer. All rights reserved.
//

#include "V82JSC.h"

using namespace v8;

#define H V82JSC_HeapObject

/**
 * Returns the global proxy object.
 *
 * Global proxy object is a thin wrapper whose prototype points to actual
 * context's global object with the properties like Object, etc. This is done
 * that way for security reasons (for more details see
 * https://wiki.mozilla.org/Gecko:SplitWindow).
 *
 * Please note that changes to global proxy object prototype most probably
 * would break VM---v8 expects only global object as a prototype of global
 * proxy object.
 */
Local<Object> Context::Global()
{
    ContextImpl *impl = V82JSC::ToContextImpl(this);
    JSObjectRef glob = JSContextGetGlobalObject(impl->m_ctxRef);
    return ValueImpl::New(impl, glob).As<Object>();
}

/**
 * Detaches the global object from its context before
 * the global object can be reused to create a new context.
 */
void Context::DetachGlobal()
{
    printf( "FIXME! Context::DetachGlobal()\n");
    //assert(0);
}

EmbedderDataImpl * get_embedder_data(const Context* cx)
{
    typedef internal::Object O;
    typedef internal::Internals I;
    O* ctx = *reinterpret_cast<O* const*>(cx);
    int embedder_data_offset = I::kContextHeaderSize +
        (internal::kApiPointerSize * I::kContextEmbedderDataIndex);
    O* embedder_data = I::ReadField<O*>(ctx, embedder_data_offset);
    if (embedder_data) {
        EmbedderDataImpl *ed = reinterpret_cast<EmbedderDataImpl*>(reinterpret_cast<uint8_t*>(embedder_data) - internal::kHeapObjectTag);
        return ed;
    }
    return nullptr;
}

Local<Context> ContextImpl::New(Isolate *isolate, JSContextRef ctx)
{
    return V82JSC::ToIsolateImpl(isolate)->m_global_contexts[JSContextGetGlobalContext(ctx)].Get(isolate);
}

static Local<Value> GetPrototypeSkipHidden(Local<Context> context, Local<Object> thiz)
{
    JSValueRef obj = V82JSC::ToJSValueRef<Value>(thiz, context);
    JSContextRef ctx = V82JSC::ToContextRef(context);
    JSValueRef our_proto = V82JSC::GetRealPrototype(context, (JSObjectRef)obj);
    
    // If our prototype is hidden, propogate
    if (JSValueIsObject(ctx, our_proto)) {
        TrackedObjectImpl *wrap = getPrivateInstance(ctx, (JSObjectRef)our_proto);
        if (wrap && wrap->m_isHiddenPrototype) {
            return GetPrototypeSkipHidden(context, ValueImpl::New(V82JSC::ToContextImpl(context), our_proto).As<Object>());
        }
    }
    // Our prototype is not hidden
    return ValueImpl::New(V82JSC::ToContextImpl(context), our_proto);
}

static bool SetPrototypeSkipHidden(Local<Context> context, Local<Object> thiz, Local<Value> prototype)
{
    JSValueRef obj = V82JSC::ToJSValueRef<Value>(thiz, context);
    JSContextRef ctx = V82JSC::ToContextRef(context);
    JSValueRef new_proto = V82JSC::ToJSValueRef(prototype, context);
    JSValueRef our_proto = V82JSC::GetRealPrototype(context, (JSObjectRef)obj);
    // If our prototype is hidden, propogate
    bool isHidden = false;
    if (JSValueIsObject(ctx, our_proto)) {
        TrackedObjectImpl *wrap = getPrivateInstance(ctx, (JSObjectRef)our_proto);
        if (wrap && wrap->m_isHiddenPrototype) {
            return SetPrototypeSkipHidden(context, ValueImpl::New(V82JSC::ToContextImpl(context), our_proto).As<Object>(), prototype);
        }
    }
    
    bool new_proto_is_hidden = false;
    if (JSValueIsObject(ctx, new_proto)) {
        TrackedObjectImpl *wrap = getPrivateInstance(ctx, (JSObjectRef)new_proto);
        new_proto_is_hidden = wrap && wrap->m_isHiddenPrototype;
        if (new_proto_is_hidden) {
            if (JSValueIsStrictEqual(ctx, wrap->m_hidden_proxy_security, new_proto)) {
                // Don't put the hidden proxy in the prototype chain, just the underlying target object
                new_proto = wrap->m_proxy_security ? wrap->m_proxy_security : wrap->m_security;
            }
            // Save a reference to this object and propagate our own properties to it
            if (!wrap->m_hidden_children_array) {
                wrap->m_hidden_children_array = JSObjectMakeArray(ctx, 0, nullptr, 0);
            }
            JSValueRef args[] = { wrap->m_hidden_children_array, obj };
            V82JSC::exec(ctx, "_1.push(_2)", 2, args);
            V82JSC::ToImpl<HiddenObjectImpl>(ValueImpl::New(V82JSC::ToContextImpl(context), new_proto))
            ->PropagateOwnPropertiesToChild(context, (JSObjectRef)obj);
        }
    }
    
    // Our prototype is not hidden
    if (!isHidden) {
        V82JSC::SetRealPrototype(context, (JSObjectRef)obj, new_proto);
    }
    
    return new_proto_is_hidden || GetPrototypeSkipHidden(context, thiz)->StrictEquals(prototype);
}


/**
 * Creates a new context and returns a handle to the newly allocated
 * context.
 *
 * \param isolate The isolate in which to create the context.
 *
 * \param extensions An optional extension configuration containing
 * the extensions to be installed in the newly created context.
 *
 * \param global_template An optional object template from which the
 * global object for the newly created context will be created.
 *
 * \param global_object An optional global object to be reused for
 * the newly created context. This global object must have been
 * created by a previous call to Context::New with the same global
 * template. The state of the global object will be completely reset
 * and only object identify will remain.
 */
Local<Context> Context::New(Isolate* isolate, ExtensionConfiguration* extensions,
                          MaybeLocal<ObjectTemplate> global_template,
                          MaybeLocal<Value> global_object)
{
    IsolateImpl * i = V82JSC::ToIsolateImpl(isolate);
    ContextImpl * context = static_cast<ContextImpl *>(H::HeapAllocator::Alloc(i, i->m_context_map));
    
    assert(H::ToHeapPointer(context)->IsContext());

    Local<Context> ctx = V82JSC::CreateLocal<Context>(isolate, context);
    int hash = 0;
    
    if (!global_object.IsEmpty()) {
        hash = global_object.ToLocalChecked().As<Object>()->GetIdentityHash();
    }

    ctx->Enter();

    if (!global_template.IsEmpty()) {
        ObjectTemplateImpl *impl = V82JSC::ToImpl<ObjectTemplateImpl>(*global_template.ToLocalChecked());
        LocalException exception(V82JSC::ToIsolateImpl(isolate));
        
        JSClassDefinition def = kJSClassDefinitionEmpty;
        def.initialize = [](JSContextRef ctx, JSObjectRef global)
        {
        };
        JSClassRef claz = JSClassCreate(&def);
        context->m_ctxRef = JSGlobalContextCreateInGroup(i->m_group, claz);
        JSClassRelease(claz);
        JSContextRef ctxRef = context->m_ctxRef;
        IsolateImpl::s_context_to_isolate_map[context->m_ctxRef] = i;
        i->m_exec_maps[context->m_ctxRef] = std::map<const char*, JSObjectRef>();

        JSObjectRef instance = JSObjectMake(ctxRef, 0, nullptr);
        auto ctortempl = Local<FunctionTemplate>::New(isolate, impl->m_constructor_template);
        
        if (!ctortempl.IsEmpty()) {
            MaybeLocal<Function> ctor = ctortempl->GetFunction(ctx);
            if (!ctor.IsEmpty()) {
                JSObjectRef ctor_func = (JSObjectRef) V82JSC::ToJSValueRef(ctor.ToLocalChecked(), ctx);
                JSStringRef sprototype = JSStringCreateWithUTF8CString("prototype");
                JSStringRef sconstructor = JSStringCreateWithUTF8CString("constructor");
                JSValueRef excp = 0;
                JSValueRef prototype = JSObjectGetProperty(ctxRef, ctor_func, sprototype, &excp);
                assert(excp == 0);
                JSObjectSetPrototype(ctxRef, instance, prototype);
                JSObjectSetProperty(ctxRef, instance, sconstructor, ctor_func, kJSPropertyAttributeDontEnum, &excp);
                assert(excp == 0);
                JSStringRelease(sprototype);
                JSStringRelease(sconstructor);
            }
        }
        MaybeLocal<Object> thiz = impl->NewInstance(ctx, instance, false);
        TrackedObjectImpl *wrap = getPrivateInstance(context->m_ctxRef, instance);
        if (hash) {
            wrap->m_hash = hash;
        }
        wrap->m_isGlobalObject = true;
        
        // Don't use V82JSC::SetRealPrototype() or GetRealPrototype() here because we haven't set them up yet,
        // but otherwise, always use them over the JSC equivalents.  The JSC API legacy functions get confused
        // by proxies
        JSObjectRef global = (JSObjectRef) JSObjectGetPrototype(context->m_ctxRef, JSContextGetGlobalObject(context->m_ctxRef));
        JSObjectSetPrototype(context->m_ctxRef, global, V82JSC::ToJSValueRef(thiz.ToLocalChecked(), ctx));

        if (exception.ShouldThow()) {
            ctx->Exit();
            return Local<Context>();
        }
    } else {
        JSClassDefinition def = kJSClassDefinitionEmpty;
        JSClassRef claz = JSClassCreate(&def);
        context->m_ctxRef = JSGlobalContextCreateInGroup(i->m_group, claz);
        IsolateImpl::s_context_to_isolate_map[context->m_ctxRef] = i;
        JSClassRelease(claz);
    }
    
    JSObjectRef global_o = JSContextGetGlobalObject(context->m_ctxRef);
    // Set a reference back to our context so we can find our way back to the creation context
    JSObjectSetPrivate(global_o, (void*)context->m_ctxRef);
    assert(JSObjectGetPrivate(global_o) == (void*)context->m_ctxRef);
    
    Copyable(Context) persistent = Copyable(Context)(isolate, ctx);
    //persistent.SetWeak();
    i->m_global_contexts[(JSGlobalContextRef)context->m_ctxRef] = std::move(persistent);

    // Don't do anything fancy if we are setting up the default context
    if (!i->m_nullContext.IsEmpty()) {
        proxyArrayBuffer(context);
        
        // Proxy Object.getPrototypeOf and Object.setPrototypeOf
        Local<FunctionTemplate> setPrototypeOf = FunctionTemplate::New(
            isolate,
            [](const FunctionCallbackInfo<Value>& info) {
                Local<Object> obj = info[0].As<Object>();
                Local<Value> proto = info[1];
                JSContextRef ctx = V82JSC::ToContextRef(info.GetIsolate());
                Local<Context> context = info.GetIsolate()->GetCurrentContext();
                JSValueRef o = V82JSC::ToJSValueRef(info[0], context);

                if (JSValueIsObject(ctx, o)) {
                    JSGlobalContextRef gctx = (JSGlobalContextRef) JSObjectGetPrivate((JSObjectRef)o);
                    if (gctx && V82JSC::ToIsolateImpl(info.GetIsolate())->m_global_contexts.count(gctx)) {
                        Local<Context> other = V82JSC::ToIsolateImpl(info.GetIsolate())->
                            m_global_contexts[gctx].Get(info.GetIsolate());
                        bool isGlobal = other->Global()->StrictEquals(info[0]);
                        if (isGlobal && !context->Global()->StrictEquals(info[0])) {
                            info.GetReturnValue().Set(False(info.GetIsolate()));
                            return;
                        }
                    }
                }

                if (SetPrototypeSkipHidden(info.GetIsolate()->GetCurrentContext(), obj, proto)) {
                    info.GetReturnValue().Set(True(info.GetIsolate()));
                } else {
                    info.GetReturnValue().Set(False(info.GetIsolate()));
                }
            });
        Local<FunctionTemplate> getPrototypeOf = FunctionTemplate::New(
            isolate,
            [](const FunctionCallbackInfo<Value>& info) {
                JSContextRef ctx = V82JSC::ToContextRef(info.GetIsolate());
                Local<Context> context = info.GetIsolate()->GetCurrentContext();
                JSValueRef o = V82JSC::ToJSValueRef(info[0], context);
                
                if (JSValueIsObject(ctx, o)) {
                    JSGlobalContextRef gctx = (JSGlobalContextRef) JSObjectGetPrivate((JSObjectRef)o);
                    if (gctx && V82JSC::ToIsolateImpl(info.GetIsolate())->m_global_contexts.count(gctx)) {
                        Local<Context> other = V82JSC::ToIsolateImpl(info.GetIsolate())->m_global_contexts[gctx].Get(info.GetIsolate());
                        bool isGlobal = other->Global()->StrictEquals(info[0]);
                        if (isGlobal && !context->Global()->StrictEquals(info[0])) {
                            info.GetReturnValue().Set(Null(info.GetIsolate()));
                            return;
                        }
                    }
                }

                Local<Object> obj = info[0].As<Object>();
                info.GetReturnValue().Set(GetPrototypeSkipHidden(info.GetIsolate()->GetCurrentContext(), obj));
            });
        Local<Object> object = ctx->Global()->Get(ctx,
            String::NewFromUtf8(isolate, "Object", NewStringType::kNormal).ToLocalChecked()).ToLocalChecked().As<Object>();
        Local<String> SsetPrototypeOf = String::NewFromUtf8(isolate, "setPrototypeOf", NewStringType::kNormal).ToLocalChecked();
        Local<String> SgetPrototypeOf = String::NewFromUtf8(isolate, "getPrototypeOf", NewStringType::kNormal).ToLocalChecked();

        context->ObjectSetPrototypeOf.Reset(isolate, object->Get(ctx, SsetPrototypeOf).ToLocalChecked().As<Function>());
        context->ObjectGetPrototypeOf.Reset(isolate, object->Get(ctx, SgetPrototypeOf).ToLocalChecked().As<Function>());
        CHECK(object->Set(ctx,
                          SsetPrototypeOf,
                          setPrototypeOf->GetFunction(ctx).ToLocalChecked()).ToChecked());
        CHECK(object->Set(ctx,
                          SgetPrototypeOf,
                          getPrototypeOf->GetFunction(ctx).ToLocalChecked()).ToChecked());
        
        // ... and capture all attempts to set the prototype through __proto__
        V82JSC::exec(context->m_ctxRef,
                     "Object.defineProperty( Object.prototype, '__proto__',"
                     "{"
                     "  get() { return Object.getPrototypeOf(this); },"
                     "  set(p) { return Object.setPrototypeOf(this, p); },"
                     "  enumerable: false,"
                     "  configurable: false"
                     "});", 0, nullptr);
        
        // Filter out our private symbol.  Nobody needs to see that.
        V82JSC::exec(context->m_ctxRef,
                     "var old = Object.getOwnPropertySymbols; "
                     "Object.getOwnPropertySymbols = "
                     "    (o) => old(o).filter( (s)=> s!= _1 )",
                     1, &i->m_private_symbol);
        
        // Save a reference to original Object.prototype.toString()
        JSValueRef toString = V82JSC::exec(context->m_ctxRef, "return Object.prototype.toString", 0, nullptr);
        context->ObjectPrototypeToString.Reset(isolate, ValueImpl::New(context, toString).As<Function>());
        
        // Override Function.prototype.bind()
        // All we are doing intercepting calls to bind and then calling the original bind and returning the
        // value.  But we need to ensure that the creation context of the bound object is the same as the
        // creation context of the function.
        JSValueRef bind = V82JSC::exec(context->m_ctxRef, "return Function.prototype.bind", 0, nullptr);
        context->FunctionPrototypeBind.Reset(isolate, ValueImpl::New(context, bind).As<Function>());
        Local<FunctionTemplate> bind_template = FunctionTemplate::New(isolate, [](const FunctionCallbackInfo<Value>& info) {
            Local<Context> context = info.This()->CreationContext();
            Local<Value> args[info.Length()];
            for (int i=0; i<info.Length(); i++) {
                args[i] = info[i];
            }
            MaybeLocal<Value> bound = V82JSC::ToContextImpl(context)->FunctionPrototypeBind.Get(info.GetIsolate())
                ->Call(context, info.This(), info.Length(), args);
            if (!bound.IsEmpty()) {
                info.GetReturnValue().Set(bound.ToLocalChecked());
            }
        });
        JSValueRef new_bind = V82JSC::ToJSValueRef(bind_template->GetFunction(ctx).ToLocalChecked(), ctx);
        V82JSC::exec(context->m_ctxRef, "Function.prototype.bind = _1", 1, &new_bind);
        
        std::map<std::string, bool> loaded_extensions;

        InstallAutoExtensions(ctx, loaded_extensions);
        if (extensions) {
            for (const char **extension = extensions->begin(); extension != extensions->end(); extension++) {
                if (!InstallExtension(ctx, *extension, loaded_extensions)) {
                    ctx->Exit();
                    return Local<Context>();
                }
            }
        }
    }
    
    ctx->Exit();
    
    return ctx;
}

/**
 * Create a new context from a (non-default) context snapshot. There
 * is no way to provide a global object template since we do not create
 * a new global object from template, but we can reuse a global object.
 *
 * \param isolate See v8::Context::New.
 *
 * \param context_snapshot_index The index of the context snapshot to
 * deserialize from. Use v8::Context::New for the default snapshot.
 *
 * \param embedder_fields_deserializer Optional callback to deserialize
 * internal fields. It should match the SerializeInternalFieldCallback used
 * to serialize.
 *
 * \param extensions See v8::Context::New.
 *
 * \param global_object See v8::Context::New.
 */

MaybeLocal<Context> Context::FromSnapshot(
                                        Isolate* isolate, size_t context_snapshot_index,
                                        DeserializeInternalFieldsCallback embedder_fields_deserializer,
                                        ExtensionConfiguration* extensions,
                                        MaybeLocal<Value> global_object)
{
    // Snashots are ignored
    Local<Context> ctx = New(isolate, extensions, MaybeLocal<ObjectTemplate>(), global_object);
    return MaybeLocal<Context>(ctx);
}

/**
 * Returns an global object that isn't backed by an actual context.
 *
 * The global template needs to have access checks with handlers installed.
 * If an existing global object is passed in, the global object is detached
 * from its context.
 *
 * Note that this is different from a detached context where all accesses to
 * the global proxy will fail. Instead, the access check handlers are invoked.
 *
 * It is also not possible to detach an object returned by this method.
 * Instead, the access check handlers need to return nothing to achieve the
 * same effect.
 *
 * It is possible, however, to create a new context from the global object
 * returned by this method.
 */
MaybeLocal<Object> Context::NewRemoteContext(
                                           Isolate* isolate, Local<ObjectTemplate> global_template,
                                           MaybeLocal<Value> global_object)
{
    assert(0);
    return MaybeLocal<Object>();
}

/**
 * Sets the security token for the context.  To access an object in
 * another context, the security tokens must match.
 */
void Context::SetSecurityToken(Local<Value> token)
{
    printf( "FIXME! Context::SetSecurityToken()\n" );
    //assert(0);
}

/** Restores the security token to the default value. */
void Context::UseDefaultSecurityToken()
{
    assert(0);
}

/** Returns the security token of this context.*/
Local<Value> Context::GetSecurityToken()
{
    assert(0);
    return Local<Value>();
}

/**
 * Enter this context.  After entering a context, all code compiled
 * and run is compiled and run in this context.  If another context
 * is already entered, this old context is saved so it can be
 * restored when the new context is exited.
 */
void Context::Enter()
{
    Isolate *isolate = V82JSC::ToIsolate(this);
    HandleScope scope(isolate);
    Local<Context> thiz = Local<Context>::New(isolate, this);
    V82JSC::ToIsolateImpl(isolate)->EnterContext(thiz);
}

/**
 * Exit this context.  Exiting the current context restores the
 * context that was in place when entering the current context.
 */
void Context::Exit()
{
    Isolate *isolate = V82JSC::ToIsolate(this);
    HandleScope scope(isolate);
    Local<Context> thiz = Local<Context>::New(isolate, this);
    V82JSC::ToIsolateImpl(isolate)->ExitContext(thiz);
}

/** Returns an isolate associated with a current context. */
Isolate* Context::GetIsolate()
{
    return V82JSC::ToIsolate(this);
}

/**
 * Gets the binding object used by V8 extras. Extra natives get a reference
 * to this object and can use it to "export" functionality by adding
 * properties. Extra natives can also "import" functionality by accessing
 * properties added by the embedder using the V8 API.
 */
Local<Object> Context::GetExtrasBindingObject()
{
    assert(0);
    return Local<Object>();
}

template<typename T>
static void WriteField(internal::Object* ptr, int offset, T value) {
    uint8_t* addr =
        reinterpret_cast<uint8_t*>(ptr) + offset - internal::kHeapObjectTag;
    *reinterpret_cast<T*>(addr) = value;
}

/**
 * Sets the embedder data with the given index, growing the data as
 * needed. Note that index 0 currently has a special meaning for Chrome's
 * debugger.
 */
void Context::SetEmbedderData(int index, Local<Value> value)
{
    Isolate *isolate = V82JSC::ToIsolate(this);
    IsolateImpl *i = V82JSC::ToIsolateImpl(isolate);
    EmbedderDataImpl *ed = get_embedder_data(this);
    
    if (!ed || ed->m_size <= index) {
        int buffer = H::ReserveSize((index+1) * sizeof(internal::Object*) + sizeof(H::FixedArray));
        int size = (buffer - sizeof(H::FixedArray)) / sizeof(internal::Object*);
        EmbedderDataImpl * newed =
            reinterpret_cast<EmbedderDataImpl *>
            (H::HeapAllocator::Alloc(i, i->m_fixed_array_map,
                                     sizeof(H::FixedArray) + size*sizeof(internal::Object *)));
        if (ed) {
            memcpy(newed->m_elements, ed->m_elements, ed->m_size * sizeof(internal::Object*));
        }
        newed->m_size = size;
        ed = newed;
    }
    
    internal::Object * val = *reinterpret_cast<internal::Object* const*>(*value);
    ed->m_elements[index] = val;
    
    Local<v8::EmbeddedFixedArray> fa = V82JSC::CreateLocal<v8::EmbeddedFixedArray>(isolate, ed);
    ContextImpl *context = V82JSC::ToContextImpl(this);
    context->m_embedder_data.Reset(isolate, fa);
    
    int embedder_data_offset = internal::Internals::kContextHeaderSize +
        (internal::kApiPointerSize * internal::Internals::kContextEmbedderDataIndex);
    internal::Object * ctx = *reinterpret_cast<internal::Object* const*>(this);
    internal::Object *embedder_data = reinterpret_cast<internal::Object*>(reinterpret_cast<intptr_t>(ed) + internal::kHeapObjectTag);
    WriteField<internal::Object*>(ctx, embedder_data_offset, embedder_data);
}

/**
 * Sets a 2-byte-aligned native pointer in the embedder data with the given
 * index, growing the data as needed. Note that index 0 currently has a
 * special meaning for Chrome's debugger.
 */
void Context::SetAlignedPointerInEmbedderData(int index, void* value)
{
    value = reinterpret_cast<void*>(reinterpret_cast<intptr_t>(value) & ~1);
    class FakeLocal {
    public:
        Value* val_;
    };
    FakeLocal fl;
    
    fl.val_ = reinterpret_cast<Value*>(&value);

    Local<Value> * v = reinterpret_cast<Local<Value>*>(&fl);
    SetEmbedderData(index, *v);
}

/**
 * Control whether code generation from strings is allowed. Calling
 * this method with false will disable 'eval' and the 'Function'
 * constructor for code running in this context. If 'eval' or the
 * 'Function' constructor are used an exception will be thrown.
 *
 * If code generation from strings is not allowed the
 * V8::AllowCodeGenerationFromStrings callback will be invoked if
 * set before blocking the call to 'eval' or the 'Function'
 * constructor. If that callback returns true, the call will be
 * allowed, otherwise an exception will be thrown. If no callback is
 * set an exception will be thrown.
 */
void Context::AllowCodeGenerationFromStrings(bool allow)
{
    assert(0);
}

/**
 * Returns true if code generation from strings is allowed for the context.
 * For more details see AllowCodeGenerationFromStrings(bool) documentation.
 */
bool Context::IsCodeGenerationFromStringsAllowed()
{
    assert(0);
    return false;
}

/**
 * Sets the error description for the exception that is thrown when
 * code generation from strings is not allowed and 'eval' or the 'Function'
 * constructor are called.
 */
void Context::SetErrorMessageForCodeGenerationFromStrings(Local<String> message)
{
    assert(0);
}
