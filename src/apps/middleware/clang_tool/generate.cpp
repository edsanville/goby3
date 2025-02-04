// Copyright 2020-2022:
//   GobySoft, LLC (2013-)
//   Community contributors (see AUTHORS file)
// File authors:
//   Toby Schneider <toby@gobysoft.org>
//
//
// This file is part of the Goby Underwater Autonomy Project Binaries
// ("The Goby Binaries").
//
// The Goby Binaries are free software: you can redistribute them and/or modify
// them under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 2 of the License, or
// (at your option) any later version.
//
// The Goby Binaries are distributed in the hope that they will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with Goby.  If not, see <http://www.gnu.org/licenses/>.

#include <functional> // for function
#include <iostream>   // for operator<<, basic...
#include <iterator>   // for operator!=, rever...
#include <map>        // for _Rb_tree_const_it...
#include <memory>     // for allocator, unique...
#include <set>        // for set
#include <stdexcept>  // for runtime_error
#include <stdlib.h>   // for exit, EXIT_FAILURE
#include <string>     // for string, basic_string
#include <utility>    // for pair, make_pair

#include <boost/algorithm/string/erase.hpp>        // for erase_all
#include <boost/iterator/iterator_traits.hpp>      // for iterator_value<>:...
#include <clang/AST/DeclCXX.h>                     // for CXXRecordDecl
#include <clang/AST/Expr.h>                        // for Expr, IntegerLiteral
#include <clang/AST/ExprCXX.h>                     // for CXXMemberCallExpr
#include <clang/AST/PrettyPrinter.h>               // for PrintingPolicy
#include <clang/AST/TemplateBase.h>                // for TemplateArgument
#include <clang/AST/Type.h>                        // for QualType, Type
#include <clang/ASTMatchers/ASTMatchersInternal.h> // for VariadicFunction
#include <clang/Basic/LangOptions.h>               // for LangOptions
#include <llvm/ADT/APInt.h>                        // for APInt
#include <llvm/ADT/APSInt.h>                       // for APSInt
#include <llvm/ADT/StringRef.h>                    // for StringRef
#include <llvm/Support/raw_ostream.h>              // for raw_string_ostream
#include <yaml-cpp/emitter.h>                      // for operator<<, Emitter

#include "actions.h"                               // for generate
#include "goby/middleware/marshalling/interface.h" // for MarshallingScheme
#include "goby/middleware/transport/interface.h"   // for Necessity, Necess...
#include "pubsub_entry.h"                          // for Layer, PubSubEntry
#include "yaml_raii.h"                             // for YMap, YSeq
#include "clang/ASTMatchers/ASTMatchFinder.h"      // for MatchFinder::Matc...
#include "clang/ASTMatchers/ASTMatchers.h"         // for hasName, callee
#include "clang/Tooling/Tooling.h"                 // for newFrontendAction...

using goby::clang::Layer;
using goby::clang::PubSubEntry;

std::map<Layer, std::string> layer_to_str{{Layer::UNKNOWN, "unknown"},
                                          {Layer::INTERTHREAD, "interthread"},
                                          {Layer::INTERPROCESS, "interprocess"},
                                          {Layer::INTERMODULE, "intermodule"},
                                          {Layer::INTERVEHICLE, "intervehicle"}};

::clang::ast_matchers::StatementMatcher pubsub_matcher(std::string method)
{
    using namespace clang::ast_matchers;

    // picks out the object (goby::middleware::Thread) that made the publish/subscribe call
    auto calling_thread_matcher = on(expr(
        anyOf(
            // Thread: pull out what "this" in "this->interprocess()" when "this" is derived from goby::middleware::Thread
            cxxMemberCallExpr(on(hasType(
                pointsTo(hasUnqualifiedDesugaredType(recordType(hasDeclaration(cxxRecordDecl(
                    cxxRecordDecl().bind("on_thread_decl"),
                    isDerivedFrom(cxxRecordDecl(hasName("::goby::middleware::Thread"))))))))))),
            // Thread: match this->goby().interprocess() or similar chains
            hasDescendant(cxxMemberCallExpr(on(hasType(pointsTo(cxxRecordDecl(
                cxxRecordDecl().bind("on_indirect_thread_decl"),
                isDerivedFrom(cxxRecordDecl(hasName("::goby::middleware::Thread"))))))))),
            // also allow direct calls to publish/subscribe
            expr().bind("on_expr")),
        // call is on an instantiation of a class derived from StaticTransporterInterface
        hasType(hasUnqualifiedDesugaredType(recordType(hasDeclaration(cxxRecordDecl(
            decl().bind("on_type_decl"),
            isDerivedFrom(cxxRecordDecl(hasName("::goby::middleware::StaticTransporterInterface"))),
            unless(hasName("::goby::middleware::NullTransporter")))))))));

    // picks out string argument to Group constructor
    auto group_string_arg_matcher =
        anyOf(stringLiteral().bind("group_string_arg"),
              declRefExpr(hasDeclaration(
                  varDecl(hasDescendant(stringLiteral().bind("group_string_arg"))))));
    // picks out integer argument to Group constructor
    auto group_int_arg_matcher = anyOf(
        integerLiteral().bind("group_int_arg"),
        declRefExpr(hasDeclaration(varDecl(hasDescendant(integerLiteral().bind("group_int_arg"))))),
        expr().bind("group_int_arg"));

    // picks out the parameters of the publish/subscribe call
    auto base_pubsub_parameters_matcher = cxxMethodDecl(
        // "publish" or "subscribe"
        hasName(method),
        // Group (must refer to goby::middleware::Group)
        hasTemplateArgument(
            0, templateArgument(refersToDeclaration(varDecl(
                   decl().bind("group_decl"),
                   hasType(cxxRecordDecl(hasName("::goby::middleware::Group"))),
                   // find the actual group argument and bind it
                   hasDescendant(cxxConstructExpr(
                       hasArgument(0, anyOf(group_string_arg_matcher, group_int_arg_matcher)),
                       hasArgument(1, group_int_arg_matcher))))))),
        // Type (no restrictions)
        hasTemplateArgument(1, templateArgument().bind("type_arg")),
        // Scheme (must be int)
        hasTemplateArgument(2, templateArgument(templateArgument().bind("scheme_arg"),
                                                refersToIntegralType(qualType(asString("int"))))));

    // if on_thread_decl or on_thread_direct_decl fails, use this to try to determine the class that contains this statement
    auto containing_class_matcher =
        anyOf(hasAncestor(cxxRecordDecl().bind(
                  "containing_class_decl")), // should be "optionally" but this isn't in clang 6
              expr());

    if (method == "publish")
    {
        auto publish_parameters_matcher = callee(base_pubsub_parameters_matcher);
        return cxxMemberCallExpr(expr().bind("pubsub_call_expr"), calling_thread_matcher,
                                 publish_parameters_matcher, containing_class_matcher);
    }
    else if (method == "subscribe")
    {
        // subscribe also has a simplified template version that infers arguments from the function parameters; we need to match the subsequent call to the full subscribe
        auto subscribe_parameters_matcher_base =
            cxxMethodDecl(base_pubsub_parameters_matcher,
                          hasTemplateArgument(3, templateArgument().bind("necessity_arg")));

        auto subscribe_parameters_matcher = anyOf(
            callee(subscribe_parameters_matcher_base),
            // matches "template <const Group& group, typename Func> void subscribe(Func f)"
            callee(cxxMethodDecl(
                hasName(method),
                // simplified version calls full subscribe, so pull the parameters out from this call
                hasDescendant(cxxMemberCallExpr(callee(subscribe_parameters_matcher_base))))));

        return cxxMemberCallExpr(expr().bind("pubsub_call_expr"), calling_thread_matcher,
                                 subscribe_parameters_matcher, containing_class_matcher);
    }
    else if (method == "subscribe_type_regex")
    {
        auto subscribe_regex_parameters_matcher = callee(base_pubsub_parameters_matcher);
        return cxxMemberCallExpr(expr().bind("pubsub_call_expr"), calling_thread_matcher,
                                 subscribe_regex_parameters_matcher, containing_class_matcher);
    }
    else
    {
        throw(
            std::runtime_error("method must be 'publish', 'subscribe', or 'subscribe_type_regex'"));
    }
}

class PubSubAggregator : public ::clang::ast_matchers::MatchFinder::MatchCallback
{
  public:
    virtual void run(const ::clang::ast_matchers::MatchFinder::MatchResult& Result)
    {
        // we know that this call is from a goby::middleware::Thread
        bool thread_is_known = true;

        // the function call itself (e.g. interprocess().publish<...>(...));
        const auto* pubsub_call_expr =
            Result.Nodes.getNodeAs<clang::CXXMemberCallExpr>("pubsub_call_expr");

        const auto* group_string_lit =
            Result.Nodes.getNodeAs<clang::StringLiteral>("group_string_arg");
        const auto* group_int_lit = Result.Nodes.getNodeAs<clang::IntegerLiteral>("group_int_arg");

        //        const auto* group_decl = Result.Nodes.getNodeAs<clang::Decl>("group_decl");

        const auto* type_arg = Result.Nodes.getNodeAs<clang::TemplateArgument>("type_arg");
        const auto* scheme_arg = Result.Nodes.getNodeAs<clang::TemplateArgument>("scheme_arg");
        const auto* necessity_arg =
            Result.Nodes.getNodeAs<clang::TemplateArgument>("necessity_arg");
        const auto* on_type_decl = Result.Nodes.getNodeAs<clang::CXXRecordDecl>("on_type_decl");
        const auto* on_thread_decl = Result.Nodes.getNodeAs<clang::CXXRecordDecl>("on_thread_decl");

        // const auto* on_expr = Result.Nodes.getNodeAs<clang::Expr>("on_expr");

        const auto* containing_class_decl =
            Result.Nodes.getNodeAs<clang::CXXRecordDecl>("containing_class_decl");

        if (!on_thread_decl)
            on_thread_decl =
                Result.Nodes.getNodeAs<clang::CXXRecordDecl>("on_indirect_thread_decl");

        if (!on_thread_decl)
        {
            thread_is_known = false;
            on_thread_decl = containing_class_decl;
        }

        if (!pubsub_call_expr || (!group_string_lit && !group_int_lit) || !on_type_decl)
            return;

        const std::string layer_type = on_type_decl->getQualifiedNameAsString();
        Layer layer = Layer::UNKNOWN;

        if (layer_type.find("InterThread") != std::string::npos)
            layer = Layer::INTERTHREAD;
        else if (layer_type.find("InterProcess") != std::string::npos)
            layer = Layer::INTERPROCESS;
        else if (layer_type.find("InterModule") != std::string::npos)
            layer = Layer::INTERMODULE;
        else if (layer_type.find("InterVehicle") != std::string::npos)
            layer = Layer::INTERVEHICLE;

        std::string thread = "unknown";
        std::set<std::string> bases;
        if (on_thread_decl)
        {
            thread = as_string(*on_thread_decl);
            insert_bases(bases, on_thread_decl);
        }

        bases_[thread] = bases;

        for (auto base : bases) parents_[base].insert(thread);

        std::string group = "unknown";
        if (group_string_lit)
            group = group_string_lit->getString().str();
        else
            group.clear();

        std::string group_int;
        if (group_int_lit)
        {
#if LLVM_VERSION_MAJOR >= 13
            group_int = llvm::toString(group_int_lit->getValue(), /*radix*/ 10, /*signed*/ false);
#else
            group_int = group_int_lit->getValue().toString(/*radix*/ 10, /*signed*/ false);
#endif
        }
        else
        {
            const auto* group_int_expr = Result.Nodes.getNodeAs<clang::Expr>("group_int_arg");
            if (group_int_expr)
            {
                llvm::raw_string_ostream os(group_int);
                group_int_expr->printPretty(os, NULL, clang::PrintingPolicy(clang::LangOptions()));
                os.flush();
                boost::erase_all(group_int, " ");
            }
        }

        if (!group_int.empty())
        {
            if (!group.empty())
                group += ";";
            group += group_int;
        }

        std::string type = "unknown";
        if (type_arg)
            type = as_string(*type_arg->getAsType());

        std::string scheme = "unknown";
        if (scheme_arg)
        {
            const int scheme_num = scheme_arg->getAsIntegral().getExtValue();
            scheme = goby::middleware::MarshallingScheme::to_string(scheme_num);
        }

        auto necessity = goby::middleware::Necessity::OPTIONAL;
        if (necessity_arg)
            necessity = static_cast<goby::middleware::Necessity>(
                necessity_arg->getAsIntegral().getExtValue());

        auto method = pubsub_call_expr->getMethodDecl()->getNameAsString();
        bool is_regex = (method.find("regex") != std::string::npos);

        auto direction = (method.find("publish") != std::string::npos)
                             ? goby::clang::PubSubEntry::Direction::PUBLISH
                             : goby::clang::PubSubEntry::Direction::SUBSCRIBE;

        std::set<std::string> internal_groups{
            "goby::middleware::interprocess::to_portal",
            "goby::middleware::interprocess::regex",
            "goby::middleware::SerializationUnSubscribeAll",
            "goby::middleware::Thread::joinable",
            "goby::middleware::Thread::shutdown",
            "goby::middleware::intervehicle::modem_data_in",
            "goby::middleware::intervehicle::modem_data_out",
            "goby::middleware::intervehicle::metadata_request",
            "goby::middleware::intervehicle::modem_ack_in",
            "goby::middleware::intervehicle::modem_expire_in",
            "goby::middleware::intervehicle::modem_subscription_forward_tx"};

        // TODO: fix generation of internal groups (all show as "unknown")
        // until then, hide them
        if (internal_groups.count(group))
            return;

        entries_.emplace(layer, direction, thread, group, scheme, type, thread_is_known, necessity,
                         is_regex);
    }

    const std::set<PubSubEntry>& entries() const { return entries_; }
    const std::set<std::string>& bases(const std::string& thread) { return bases_[thread]; }
    const std::set<std::string>& parents(const std::string& thread) { return parents_[thread]; }

  private:
    std::string as_string(const clang::Type& type)
    {
        // todo: see if there's a cleaner way to get this with the template parameters
        std::string str(type.getCanonicalTypeInternal().getAsString());

        // remove class, struct
        std::string class_str = "class ";
        if (str.find(class_str) == 0)
            str = str.substr(class_str.size());

        std::string struct_str = "struct ";
        if (str.find(struct_str) == 0)
            str = str.substr(struct_str.size());

        return str;
    }

    std::string as_string(const clang::CXXRecordDecl& cxx_decl)
    {
        return as_string(*(cxx_decl.getTypeForDecl()));
    }

    // recursively insert all base classes
    void insert_bases(std::set<std::string>& bases, const clang::CXXRecordDecl* thread_decl)
    {
        std::string thread = as_string(*thread_decl);
        for (auto it = thread_decl->bases_begin(), end = thread_decl->bases_end(); it != end; ++it)
        {
            bases.insert(as_string(*(it->getType()->getAsCXXRecordDecl())));
            insert_bases(bases, it->getType()->getAsCXXRecordDecl());
        }
    }

  private:
    std::set<PubSubEntry> entries_;
    // map thread to bases
    std::map<std::string, std::set<std::string>> bases_;
    std::map<std::string, std::set<std::string>> parents_;
};

int goby::clang::generate(::clang::tooling::ClangTool& Tool, std::string output_directory,
                          std::string output_file, std::string target_name)
{
    PubSubAggregator publish_aggregator, subscribe_aggregator;
    ::clang::ast_matchers::MatchFinder finder;

    finder.addMatcher(pubsub_matcher("publish"), &publish_aggregator);
    finder.addMatcher(pubsub_matcher("subscribe"), &subscribe_aggregator);
    finder.addMatcher(pubsub_matcher("subscribe_type_regex"), &subscribe_aggregator);

    if (output_file.empty())
        output_file = target_name + "_interface.yml";

    std::string file_name(output_directory + "/" + output_file);
    std::ofstream ofs(file_name.c_str());
    if (!ofs.is_open())
    {
        std::cerr << "Failed to open " << file_name << " for writing" << std::endl;
        exit(EXIT_FAILURE);
    }

    auto retval = Tool.run(::clang::tooling::newFrontendActionFactory(&finder).get());

    std::set<Layer> layers_in_use;
    std::set<std::pair<std::string, bool>> threads_in_use;
    for (const auto& e : publish_aggregator.entries())
    {
        layers_in_use.insert(e.layer);

        // only include most derived thread class
        if (publish_aggregator.parents(e.thread).empty() &&
            subscribe_aggregator.parents(e.thread).empty())
            threads_in_use.insert(std::make_pair(e.thread, e.thread_is_known));
    }
    for (const auto& e : subscribe_aggregator.entries())
    {
        layers_in_use.insert(e.layer);
        if (publish_aggregator.parents(e.thread).empty() &&
            subscribe_aggregator.parents(e.thread).empty())
            threads_in_use.insert(std::make_pair(e.thread, e.thread_is_known));
    }

    // intervehicle requires interprocess at this point
    if (layers_in_use.count(Layer::INTERVEHICLE))
        layers_in_use.insert(Layer::INTERPROCESS);

    // add interthread so that we can get bases even if there's no interthread pubsub actually happening
    if (layers_in_use.count(Layer::INTERPROCESS))
        layers_in_use.insert(Layer::INTERTHREAD);

    YAML::Emitter yaml_out;
    {
        goby::yaml::YMap root_map(yaml_out);
        root_map.add("application", std::string(target_name));

        // put inner most layer last
        for (auto layer_it = layers_in_use.rbegin(), end = layers_in_use.rend(); layer_it != end;
             ++layer_it)
        {
            Layer layer = *layer_it;
            root_map.add_key(layer_to_str.at(layer));
            goby::yaml::YMap layer_map(yaml_out);

            auto find_most_derived_parents = [&](std::string thread) -> std::set<std::string> {
                // if e.thread has parents, use them instead
                std::set<std::string> most_derived{thread};

                bool parents_found = true;
                while (parents_found)
                {
                    std::set<std::string> new_most_derived;
                    // assume we find no parents until we do
                    parents_found = false;
                    for (const auto& thread : most_derived)
                    {
                        auto pub_parents = publish_aggregator.parents(thread);
                        auto sub_parents = subscribe_aggregator.parents(thread);
                        if (!(pub_parents.empty() && sub_parents.empty()))
                        {
                            parents_found = true;
                            new_most_derived.insert(pub_parents.begin(), pub_parents.end());
                            new_most_derived.insert(sub_parents.begin(), sub_parents.end());
                        }
                        else
                        {
                            new_most_derived.insert(thread);
                        }
                    }
                    most_derived = new_most_derived;
                }
                return most_derived;
            };

            auto emit_pub_sub = [&](goby::yaml::YMap& map,
                                    const std::set<std::string>& thread_and_bases) {
                {
                    map.add_key("publishes");
                    goby::yaml::YSeq publish_seq(yaml_out);
                    for (auto e : publish_aggregator.entries())
                    {
                        auto most_derived = find_most_derived_parents(e.thread);

                        // show inner publications
                        if (e.layer >= layer &&
                            (layer != Layer::INTERTHREAD || thread_and_bases.count(e.thread)))
                        {
                            for (const auto& thread : most_derived)
                            {
                                // overwrite with most derived
                                e.thread = thread;

                                e.write_yaml_map(yaml_out, layer != Layer::INTERTHREAD,
                                                 e.layer > layer, false);

                                // special case: Intervehicle publishes both PROTOBUF and DCCL version on inner layers
                                if (e.layer > layer && e.layer == Layer::INTERVEHICLE &&
                                    e.scheme == "DCCL")
                                {
                                    auto pb_e = e;
                                    pb_e.scheme = "PROTOBUF";
                                    pb_e.write_yaml_map(yaml_out, layer != Layer::INTERTHREAD,
                                                        pb_e.layer > layer, false);
                                }
                            }
                        }
                    }
                }

                {
                    map.add_key("subscribes");
                    goby::yaml::YSeq subscribe_seq(yaml_out);
                    for (auto e : subscribe_aggregator.entries())
                    {
                        if (e.layer == layer &&
                            (layer != Layer::INTERTHREAD || thread_and_bases.count(e.thread)))
                        {
                            auto most_derived = find_most_derived_parents(e.thread);
                            for (const auto& thread : most_derived)
                            {
                                // overwrite with most derived
                                e.thread = thread;
                                e.write_yaml_map(yaml_out, layer != Layer::INTERTHREAD);
                            }
                        }
                    }
                }
            };

            if (layer == Layer::INTERTHREAD)
            {
                layer_map.add_key("threads");
                goby::yaml::YSeq thread_seq(yaml_out);
                for (const auto& thread : threads_in_use)
                {
                    goby::yaml::YMap thread_map(yaml_out);
                    {
                        thread_map.add("name", thread.first);
                        thread_map.add("known", thread.second);
                        std::set<std::string> thread_and_bases{thread.first};

                        std::set<std::string> bases;
                        std::function<void(std::string)> add_bases_recurse =
                            [&](std::string thread) {
                                std::set<std::string> new_bases;
                                const auto& pub_bases = publish_aggregator.bases(thread);
                                const auto& sub_bases = subscribe_aggregator.bases(thread);
                                new_bases.insert(pub_bases.begin(), pub_bases.end());
                                new_bases.insert(sub_bases.begin(), sub_bases.end());
                                bases.insert(new_bases.begin(), new_bases.end());
                                for (const auto& base : new_bases) add_bases_recurse(base);
                            };

                        add_bases_recurse(thread.first);

                        if (!bases.empty())
                        {
                            thread_map.add_key("bases");
                            goby::yaml::YSeq bases_seq(yaml_out);

                            for (const auto& base : bases)
                            {
                                bases_seq.add(base);
                                thread_and_bases.insert(base);
                            }
                        }
                        emit_pub_sub(thread_map, thread_and_bases);
                    }
                }
            }
            else
            {
                emit_pub_sub(layer_map, {});
            }
        }
    }

    ofs << yaml_out.c_str();

    return retval;
}
