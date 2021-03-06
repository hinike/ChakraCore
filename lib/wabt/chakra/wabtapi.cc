//-------------------------------------------------------------------------------------------------------
// Copyright (C) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------

#include "wabtapi.h"

#pragma warning(push, 0)
#include "src/wast-parser.h"
#include "src/wast-lexer.h"
#include "src/resolve-names.h"
#include "src/binary-writer.h"
#include "src/error-handler.h"
#include "src/ir.h"
#include "src/cast.h"
#pragma warning(pop)

using namespace wabt;
using namespace ChakraWabt;

class MyErrorHandler : public ErrorHandler
{
public:
    MyErrorHandler() : ErrorHandler(Location::Type::Text) {}

    virtual bool OnError(const Location& loc, const std::string& error, const std::string& source_line, size_t source_line_column_offset) override
    {
        int colStart = loc.first_column - 1 - (int)source_line_column_offset;
        int sourceErrorLength = (loc.last_column - loc.first_column) - 2;
        if (sourceErrorLength < 0)
        {
            // -2 probably overflowed
            sourceErrorLength = 0;
        }
        char buf[4096];
        wabt_snprintf(buf, 4096, "Wast Parsing error:%u:%u:\n%s\n%s\n%*s^%*s^",
            loc.line,
            loc.first_column,
            error.c_str(),
            source_line.c_str(),
            colStart, "",
            sourceErrorLength, "");
        throw Error(buf);
    }

    virtual size_t source_line_max_length() const override
    {
        return 256;
    }

};

uint TruncSizeT(size_t value)
{
    if (value > 0xffffffff)
    {
        throw Error("Out of Memory");
    }
    return (uint)value;
}

void set_property(Context* ctx, Js::Var obj, PropertyId id, Js::Var value, const char* messageIfFailed)
{
    bool success = ctx->spec->setProperty(obj, id, value, ctx->user_data);
    if (!success)
    {
        throw Error(messageIfFailed);
    }
}

void write_int32(Context* ctx, Js::Var obj, PropertyId id, int32 value)
{
    Js::Var line = ctx->spec->int32ToVar(value, ctx->user_data);
    set_property(ctx, obj, id, line, "Unable to write number");
}

void write_int64(Context* ctx, Js::Var obj, PropertyId id, int64 value)
{
    Js::Var line = ctx->spec->int64ToVar(value, ctx->user_data);
    set_property(ctx, obj, id, line, "Unable to write number");
}

void write_string(Context* ctx, Js::Var obj, PropertyId id, const char* src, size_t length = 0xFFFFFFFF)
{
    Js::Var str = ctx->spec->stringToVar(src, TruncSizeT(length == 0xFFFFFFFF ? strlen(src) : length), ctx->user_data);
    set_property(ctx, obj, id, str, "Unable to write string");
}

void write_string(Context* ctx, Js::Var obj, PropertyId id, const std::string& src)
{
    write_string(ctx, obj, id, src.c_str(), src.size());
}

void write_location(Context* ctx, Js::Var obj, const Location* loc)
{
    write_int32(ctx, obj, PropertyIds::line, loc->line);
}

void write_var(Context* ctx, Js::Var obj, PropertyId id, const Var* var)
{
    if (var->is_index())
    {
        write_int64(ctx, obj, id, var->index());
    }
    else
    {
        assert(var->is_name());
        write_string(ctx, obj, id, var->name());
    }
}

void write_command_type(Context* ctx, CommandType type, Js::Var cmdObj)
{
    static const char* s_command_names[] = {
        "module",
        "action",
        "register",
        "assert_malformed",
        "assert_invalid",
        "assert_unlinkable",
        "assert_uninstantiable",
        "assert_return",
        "assert_return_canonical_nan",
        "assert_return_arithmetic_nan",
        "assert_trap",
        "assert_exhaustion",
    };
    WABT_STATIC_ASSERT(sizeof(s_command_names) / sizeof(char*) == (int)CommandType::Last + 1);
    uint i = (uint)type;
    if (i > (uint)CommandType::Last)
    {
        throw Error("invalid command type");
    }
    write_string(ctx, cmdObj, PropertyIds::type, s_command_names[i]);
}

Js::Var create_const_vector(Context* ctx, const ConstVector& consts)
{
    Js::Var constsArr = ctx->spec->createArray(ctx->user_data);

    size_t i;
    for (i = 0; i < consts.size(); ++i)
    {
        Js::Var constDescriptor = ctx->spec->createObject(ctx->user_data);
        ctx->spec->push(constsArr, constDescriptor, ctx->user_data);

        char buf[32];
        const Const& const_ = consts.at(i);
        switch (const_.type)
        {
        case Type::I32:
            write_string(ctx, constDescriptor, PropertyIds::type, "i32");
            wabt_snprintf(buf, 32, "%u", const_.u32);
            break;
        case Type::I64:
            write_string(ctx, constDescriptor, PropertyIds::type, "i64");
            wabt_snprintf(buf, 32, "%llu", const_.u64);
            break;
        case Type::F32:
            write_string(ctx, constDescriptor, PropertyIds::type, "f32");
            wabt_snprintf(buf, 32, "%u", const_.f32_bits);
            break;
        case Type::F64:
            write_string(ctx, constDescriptor, PropertyIds::type, "f64");
            wabt_snprintf(buf, 32, "%llu", const_.f64_bits);
            break;
        default:
            assert(0);
            throw Error("invalid constant type");
        }
        write_string(ctx, constDescriptor, PropertyIds::value, buf);
    }
    return constsArr;
}

void write_const_vector(Context* ctx, Js::Var obj, PropertyId id, const ConstVector& consts)
{
    Js::Var constsArr = create_const_vector(ctx, consts);
    set_property(ctx, obj, id, constsArr, "Unable to write const vector");
}

Js::Var create_type_object(Context* ctx, Type type)
{
    Js::Var typeObj = ctx->spec->createObject(ctx->user_data);
    write_string(ctx, typeObj, PropertyIds::type, GetTypeName(type));
    return typeObj;
}

void write_action_result_type(Context* ctx, Js::Var obj, PropertyId id, Script* script, const ActionPtr& action)
{
    const Module* module = script->GetModule(action->module_var);
    const Export* export_;
    Js::Var resultArr = ctx->spec->createArray(ctx->user_data);
    set_property(ctx, obj, id, resultArr, "Unable to set action result type");

    switch (action->type())
    {
    case ActionType::Invoke:
    {
        export_ = module->GetExport(action->name);
        assert(export_->kind == ExternalKind::Func);
        const Func* func = module->GetFunc(export_->var);
        wabt::Index num_results = func->GetNumResults();
        wabt::Index i;
        for (i = 0; i < num_results; ++i)
        {
            Js::Var typeObj = create_type_object(ctx, func->GetResultType(i));
            ctx->spec->push(resultArr, typeObj, ctx->user_data);
        }
        break;
    }

    case ActionType::Get:
    {
        export_ = module->GetExport(action->name);
        assert(export_->kind == ExternalKind::Global);
        const Global* global = module->GetGlobal(export_->var);
        Js::Var typeObj = create_type_object(ctx, global->type);
        ctx->spec->push(resultArr, typeObj, ctx->user_data);
        break;
    }
    }
}

void write_action(Context* ctx, Js::Var obj, const ActionPtr& action)
{
    Js::Var actionObj = ctx->spec->createObject(ctx->user_data);
    set_property(ctx, obj, PropertyIds::action, actionObj, "Unable to set action");

    if (action->module_var.is_name())
    {
        write_var(ctx, actionObj, PropertyIds::module, &action->module_var);
    }
    if (action->type() == ActionType::Invoke)
    {
        const InvokeAction* iaction = cast<InvokeAction>(action.get());
        write_string(ctx, actionObj, PropertyIds::type, "invoke");
        write_string(ctx, actionObj, PropertyIds::field, iaction->name);
        write_const_vector(ctx, actionObj, PropertyIds::args, iaction->args);
    }
    else
    {
        assert(action->type() == ActionType::Get);
        write_string(ctx, actionObj, PropertyIds::type, "get");
        write_string(ctx, actionObj, PropertyIds::field, action->name);
    }
}

Js::Var create_module(Context* ctx, const Module* module)
{
    if (!module)
    {
        throw Error("No module found");
    }
    MemoryStream stream;
    WriteBinaryOptions s_write_binary_options;
    Result result = WriteBinaryModule(&stream, module, &s_write_binary_options);
    if (!Succeeded(result))
    {
        throw Error("Error while writing module");
    }
    const uint8_t* data = stream.output_buffer().data.data();
    const size_t size = stream.output_buffer().size();
    return ctx->createBuffer(data, TruncSizeT(size), ctx->user_data);
}

void write_module(Context* ctx, Js::Var obj, const Module* module)
{
    Js::Var buffer = create_module(ctx, module);
    set_property(ctx, obj, PropertyIds::buffer, buffer, "Unable to set module");
}

static void write_invalid_module(Context* ctx, Js::Var obj, const ScriptModule* module, const std::string& text)
{
    write_location(ctx, obj, &module->location());
    write_string(ctx, obj, PropertyIds::text, text);
    const std::vector<uint8_t>* data = nullptr;
    switch (module->type())
    {
    case ScriptModuleType::Text:
        write_module(ctx, obj, &cast<TextScriptModule>(module)->module);
        break;
    case ScriptModuleType::Binary:
        data = &cast<BinaryScriptModule>(module)->data;
        goto create_binary_module;
    case ScriptModuleType::Quoted:
        data = &cast<QuotedScriptModule>(module)->data;
    create_binary_module:
        {
            Js::Var buffer = ctx->createBuffer(data->data(), TruncSizeT(data->size()), ctx->user_data);
            set_property(ctx, obj, PropertyIds::buffer, buffer, "Unable to set invalid module");
            break;
        }
    default:
        assert(false);
        break;
    }
}
Js::Var write_commands(Context* ctx, Script* script)
{

    Js::Var resultObj = ctx->spec->createObject(ctx->user_data);
    Js::Var commandsArr = ctx->spec->createArray(ctx->user_data);
    set_property(ctx, resultObj, PropertyIds::commands, commandsArr, "Unable to set commands");
    wabt::Index last_module_index = (wabt::Index) - 1;
    for (wabt::Index i = 0; i < script->commands.size(); ++i)
    {
        const Command* command = script->commands[i].get();

        Js::Var cmdObj = ctx->spec->createObject(ctx->user_data);
        ctx->spec->push(commandsArr, cmdObj, ctx->user_data);
        write_command_type(ctx, command->type, cmdObj);

        switch (command->type)
        {
        case CommandType::Module:
        {
            const Module& module = cast<ModuleCommand>(command)->module;
            write_location(ctx, cmdObj, &module.loc);
            if (!module.name.empty())
            {
                write_string(ctx, cmdObj, PropertyIds::name, module.name);
            }
            write_module(ctx, cmdObj, &module);
            last_module_index = i;
            break;
        }

        case CommandType::Action:
        {
            const ActionPtr& action = cast<ActionCommand>(command)->action;
            write_location(ctx, cmdObj, &action->loc);
            write_action(ctx, cmdObj, action);
            break;
        }
        case CommandType::Register:
        {
            auto* register_command = cast<RegisterCommand>(command);
            write_location(ctx, cmdObj, &register_command->var.loc);
            if (register_command->var.is_name())
            {
                write_var(ctx, cmdObj, PropertyIds::name, &register_command->var);
            }
            else
            {
                /* If we're not registering by name, then we should only be
                * registering the last module. */
                WABT_USE(last_module_index);
                assert(register_command->var.index() == last_module_index);
            }
            write_string(ctx, cmdObj, PropertyIds::as, register_command->module_name);
            break;
        }
        case CommandType::AssertMalformed:
        {
            auto* assert_malformed_command = cast<AssertMalformedCommand>(command);
            write_invalid_module(ctx, cmdObj, assert_malformed_command->module.get(),
                assert_malformed_command->text);
            break;
        }
        case CommandType::AssertInvalid:
        {
            auto* assert_invalid_command = cast<AssertInvalidCommand>(command);
            write_invalid_module(ctx, cmdObj, assert_invalid_command->module.get(),
                assert_invalid_command->text);
            break;
        }
        case CommandType::AssertUnlinkable:
        {
            auto* assert_unlinkable_command = cast<AssertUnlinkableCommand>(command);
            write_invalid_module(ctx, cmdObj, assert_unlinkable_command->module.get(),
                assert_unlinkable_command->text);
            break;
        }
        case CommandType::AssertUninstantiable:
        {
            auto* assert_uninstantiable_command = cast<AssertUninstantiableCommand>(command);
            write_invalid_module(ctx, cmdObj, assert_uninstantiable_command->module.get(),
                assert_uninstantiable_command->text);
            break;
        }
        case CommandType::AssertReturn:
        {
            auto* assert_return_command = cast<AssertReturnCommand>(command);
            write_location(ctx, cmdObj, &assert_return_command->action->loc);
            write_action(ctx, cmdObj, assert_return_command->action);
            write_const_vector(ctx, cmdObj, PropertyIds::expected, assert_return_command->expected);
            break;
        }
        case CommandType::AssertReturnCanonicalNan:
        {
            auto* assert_return_canonical_nan_command = cast<AssertReturnCanonicalNanCommand>(command);
            write_location(ctx, cmdObj, &assert_return_canonical_nan_command->action->loc);
            write_action(ctx, cmdObj, assert_return_canonical_nan_command->action);
            write_action_result_type(ctx, cmdObj, PropertyIds::expected, script,
                assert_return_canonical_nan_command->action);
            break;
        }
        case CommandType::AssertReturnArithmeticNan:
        {
            auto* assert_return_arithmetic_nan_command = cast<AssertReturnArithmeticNanCommand>(command);
            write_location(ctx, cmdObj, &assert_return_arithmetic_nan_command->action->loc);
            write_action(ctx, cmdObj, assert_return_arithmetic_nan_command->action);
            write_action_result_type(ctx, cmdObj, PropertyIds::expected, script,
                assert_return_arithmetic_nan_command->action);
            break;
        }
        case CommandType::AssertTrap:
        {
            auto* assert_trap_command = cast<AssertTrapCommand>(command);
            write_location(ctx, cmdObj, &assert_trap_command->action->loc);
            write_action(ctx, cmdObj, assert_trap_command->action);
            write_string(ctx, cmdObj, PropertyIds::text, assert_trap_command->text);
            break;
        }
        case CommandType::AssertExhaustion:
        {
            auto* assert_exhaustion_command = cast<AssertExhaustionCommand>(command);
            write_location(ctx, cmdObj, &assert_exhaustion_command->action->loc);
            write_action(ctx, cmdObj, assert_exhaustion_command->action);
            break;
        }
        }
    }
    return resultObj;
}

void Validate(const Context& ctx, bool isSpec)
{
    if (!ctx.createBuffer) throw Error("Missing createBuffer");
    if (isSpec)
    {
        if (!ctx.spec) throw Error("Missing Spec context");
        if (!ctx.spec->setProperty) throw Error("Missing spec->setProperty");
        if (!ctx.spec->int32ToVar) throw Error("Missing spec->int32ToVar");
        if (!ctx.spec->int64ToVar) throw Error("Missing spec->int64ToVar");
        if (!ctx.spec->stringToVar) throw Error("Missing spec->stringToVar");
        if (!ctx.spec->createObject) throw Error("Missing spec->createObject");
        if (!ctx.spec->createArray) throw Error("Missing spec->createArray");
        if (!ctx.spec->push) throw Error("Missing spec->push");
    }
}

void CheckResult(Result result, const char* errorMessage)
{
    if (!Succeeded(result))
    {
        throw Error(errorMessage);
    }
}

Features GetWabtFeatures(Context& ctx)
{
    Features features;
    if (ctx.features.sign_extends)
    {
        features.enable_threads();
    }
    return features;
}

Js::Var ChakraWabt::ConvertWast2Wasm(Context& ctx, char* buffer, uint bufferSize, bool isSpecText)
{
    Validate(ctx, isSpecText);

    std::unique_ptr<WastLexer> lexer = WastLexer::CreateBufferLexer("", buffer, (size_t)bufferSize);

    MyErrorHandler s_error_handler;
    WastParseOptions options(GetWabtFeatures(ctx));
    if (isSpecText)
    {
        std::unique_ptr<Script> script;
        Result result = ParseWastScript(lexer.get(), &script, &s_error_handler, &options);
        CheckResult(result, "Invalid wast script");

        result = ResolveNamesScript(lexer.get(), script.get(), &s_error_handler);
        CheckResult(result, "Unable to resolve script's names");

        return write_commands(&ctx, script.get());
    }
    else
    {
        std::unique_ptr<Module> module;
        Result result = ParseWatModule(lexer.get(), &module, &s_error_handler, &options);
        CheckResult(result, "Invalid wat script");

        result = ResolveNamesModule(lexer.get(), module.get(), &s_error_handler);
        CheckResult(result, "Unable to resolve module's names");

        return create_module(&ctx, module.get());
    }
}
