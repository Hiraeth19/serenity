/*
 * Copyright (c) 2023, Dan Klishch <danilklishch@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Format.h>
#include <LibCore/ArgsParser.h>
#include <LibMain/Main.h>

#include "Compiler/Passes/CFGBuildingPass.h"
#include "Compiler/Passes/CFGSimplificationPass.h"
#include "Compiler/Passes/DeadCodeEliminationPass.h"
#include "Compiler/Passes/FunctionCallCanonicalizationPass.h"
#include "Compiler/Passes/IfBranchMergingPass.h"
#include "Compiler/Passes/ReferenceResolvingPass.h"
#include "Compiler/Passes/SSABuildingPass.h"
#include "Function.h"
#include "Parser/CppASTConverter.h"
#include "Parser/SpecParser.h"

using namespace JSSpecCompiler;

struct CompilationStepWithDumpOptions {
    OwnPtr<CompilationStep> step;
    bool dump_ast = false;
    bool dump_cfg = false;
};

class CompilationPipeline {
public:
    template<typename T>
    void add_compilation_pass()
    {
        auto func = +[](TranslationUnitRef translation_unit) {
            T { translation_unit }.run();
        };
        add_step(adopt_own_if_nonnull(new NonOwningCompilationStep(T::name, func)));
    }

    template<typename T>
    void for_each_step_in(StringView pass_list, T&& func)
    {
        HashTable<StringView> selected_steps;
        for (auto pass : pass_list.split_view(',')) {
            if (pass == "all") {
                for (auto const& step : m_pipeline)
                    selected_steps.set(step.step->name());
            } else if (pass == "last") {
                selected_steps.set(m_pipeline.last().step->name());
            } else if (pass.starts_with('-')) {
                VERIFY(selected_steps.remove(pass.substring_view(1)));
            } else {
                selected_steps.set(pass);
            }
        }

        for (auto& step : m_pipeline)
            if (selected_steps.contains(step.step->name()))
                func(step);
    }

    void add_step(OwnPtr<CompilationStep>&& step)
    {
        m_pipeline.append({ move(step) });
    }

    auto const& pipeline() const { return m_pipeline; }

private:
    Vector<CompilationStepWithDumpOptions> m_pipeline;
};

ErrorOr<int> serenity_main(Main::Arguments arguments)
{
    Core::ArgsParser args_parser;

    StringView filename;
    args_parser.add_positional_argument(filename, "File to compile", "file");

    constexpr StringView language_spec = "spec"sv;
    constexpr StringView language_cpp = "c++"sv;
    StringView language = language_spec;
    args_parser.add_option(Core::ArgsParser::Option {
        .argument_mode = Core::ArgsParser::OptionArgumentMode::Optional,
        .help_string = "Specify the language of the input file.",
        .short_name = 'x',
        .value_name = "{c++|spec}",
        .accept_value = [&](StringView value) {
            language = value;
            return language.is_one_of(language_spec, language_cpp);
        },
    });

    StringView passes_to_dump_ast;
    args_parser.add_option(passes_to_dump_ast, "Dump AST after specified passes.", "dump-ast", 0, "{all|last|<pass-name>|-<pass-name>[,...]}");

    StringView passes_to_dump_cfg;
    args_parser.add_option(passes_to_dump_cfg, "Dump CFG after specified passes.", "dump-cfg", 0, "{all|last|<pass-name>|-<pass-name>[,...]}");

    args_parser.parse(arguments);

    CompilationPipeline pipeline;
    if (language == language_cpp)
        pipeline.add_step(adopt_own_if_nonnull(new CppParsingStep()));
    else
        pipeline.add_step(adopt_own_if_nonnull(new SpecParsingStep()));
    pipeline.add_compilation_pass<FunctionCallCanonicalizationPass>();
    pipeline.add_compilation_pass<IfBranchMergingPass>();
    pipeline.add_compilation_pass<ReferenceResolvingPass>();
    pipeline.add_compilation_pass<CFGBuildingPass>();
    pipeline.add_compilation_pass<CFGSimplificationPass>();
    pipeline.add_compilation_pass<SSABuildingPass>();
    pipeline.add_compilation_pass<DeadCodeEliminationPass>();

    pipeline.for_each_step_in(passes_to_dump_ast, [](CompilationStepWithDumpOptions& step) {
        step.dump_ast = true;
    });
    pipeline.for_each_step_in(passes_to_dump_cfg, [](CompilationStepWithDumpOptions& step) {
        step.dump_cfg = true;
    });

    TranslationUnit translation_unit;
    translation_unit.filename = filename;

    // Functions referenced in DifferenceISODate
    // TODO: This is here just for testing. In a long run, we need some place, which is not
    //       `serenity_main`, to store built-in functions.
    translation_unit.adopt_declaration(make_ref_counted<FunctionDeclaration>("CompareISODate"sv));
    translation_unit.adopt_declaration(make_ref_counted<FunctionDeclaration>("CreateDateDurationRecord"sv));
    translation_unit.adopt_declaration(make_ref_counted<FunctionDeclaration>("AddISODate"sv));
    translation_unit.adopt_declaration(make_ref_counted<FunctionDeclaration>("ISODaysInMonth"sv));
    translation_unit.adopt_declaration(make_ref_counted<FunctionDeclaration>("ISODateToEpochDays"sv));
    translation_unit.adopt_declaration(make_ref_counted<FunctionDeclaration>("truncate"sv));
    translation_unit.adopt_declaration(make_ref_counted<FunctionDeclaration>("remainder"sv));

    for (auto const& step : pipeline.pipeline()) {
        step.step->run(&translation_unit);

        if (step.dump_ast) {
            outln(stderr, "===== AST after {} =====", step.step->name());
            for (auto const& function : translation_unit.functions_to_compile) {
                outln(stderr, "{}({}):", function->m_name, function->m_argument_names);
                outln(stderr, "{}", function->m_ast);
            }
        }
        if (step.dump_cfg && translation_unit.functions_to_compile[0]->m_cfg != nullptr) {
            outln(stderr, "===== CFG after {} =====", step.step->name());
            for (auto const& function : translation_unit.functions_to_compile) {
                outln(stderr, "{}({}):", function->m_name, function->m_argument_names);
                outln(stderr, "{}", *function->m_cfg);
            }
        }
    }

    return 0;
}
