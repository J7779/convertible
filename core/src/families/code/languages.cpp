// Convertible core - code family: the languages known to the highlighter and
// the source-code readers.
//
// Adding a language: write (or reuse) a `Lexicon` and add one row to
// `language_table`. The row's id becomes a format id with its extensions,
// shebang interpreters and well-known file names used for detection.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <array>
#include <string_view>

#include "families/code/highlight.hpp"
#include "util/text.hpp"

namespace cvt::code {

namespace {

// ---------------------------------------------------------------------------- C family

constexpr std::string_view c_keywords =
    "auto break case const continue default do else enum extern for goto if inline register restrict return sizeof "
    "static struct switch typedef union volatile while _Alignas _Alignof _Atomic _Generic _Noreturn _Static_assert "
    "_Thread_local alignas alignof static_assert thread_local typeof constexpr";
constexpr std::string_view c_types =
    "char double float int long short signed unsigned void bool _Bool _Complex size_t ssize_t ptrdiff_t intptr_t "
    "uintptr_t int8_t int16_t int32_t int64_t uint8_t uint16_t uint32_t uint64_t wchar_t char16_t char32_t FILE "
    "va_list off_t time_t";
constexpr std::string_view c_constants = "NULL true false EOF stdin stdout stderr nullptr";

constexpr Lexicon c_lex{
    .line_comments = "//", .block_comments = "/* */", .quotes = "\"'", .hash_preprocessor = true,
    .upper_constants = true, .keywords = c_keywords, .types = c_types, .constants = c_constants,
};

constexpr Lexicon cpp_lex{
    .line_comments = "//", .block_comments = "/* */", .quotes = "\"'", .cpp_raw_strings = true,
    .hash_preprocessor = true, .upper_constants = true,
    .keywords = "alignas alignof and and_eq asm auto bitand bitor break case catch class compl concept const consteval "
                "constexpr constinit const_cast continue co_await co_return co_yield decltype default delete do "
                "dynamic_cast else enum explicit export extern final for friend goto if import inline module mutable "
                "namespace new noexcept not not_eq operator or or_eq override private protected public register "
                "reinterpret_cast requires return sizeof static static_assert static_cast struct switch template this "
                "thread_local throw try typedef typeid typename union using virtual volatile while xor xor_eq",
    .types = "bool char char8_t char16_t char32_t double float int long short signed unsigned void wchar_t size_t "
             "ptrdiff_t int8_t int16_t int32_t int64_t uint8_t uint16_t uint32_t uint64_t intptr_t uintptr_t string "
             "string_view vector map unordered_map set array span optional variant unique_ptr shared_ptr",
    .constants = "nullptr true false NULL",
    .builtins = "std",
};

constexpr Lexicon objc_lex{
    .line_comments = "//", .block_comments = "/* */", .quotes = "\"'", .hash_preprocessor = true,
    .at_annotations = true, .capitalized_types = true, .upper_constants = true,
    .keywords = "auto break case const continue default do else enum extern for goto if inline register return sizeof "
                "static struct switch typedef union volatile while self super in out inout bycopy byref oneway "
                "@interface @implementation @end @property @synthesize @dynamic @protocol @class @selector @encode "
                "@try @catch @finally @throw @autoreleasepool nonatomic atomic strong weak copy assign readonly "
                "readwrite nullable nonnull class namespace template public private protected virtual new delete",
    .types = c_types,
    .constants = "nil Nil YES NO NULL true false",
};

constexpr Lexicon csharp_lex{
    .line_comments = "//", .block_comments = "/* */", .quotes = "\"'", .triple_quotes = true,
    .hash_preprocessor = true, .capitalized_types = true,
    .keywords = "abstract as async await base break case catch checked class const continue default delegate do else "
                "enum event explicit extern finally fixed for foreach get goto if implicit in init interface internal "
                "is lock namespace new operator out override params partial private protected public readonly record "
                "ref required return sealed set sizeof stackalloc static struct switch this throw try typeof "
                "unchecked unsafe using var virtual volatile when where while with yield nameof global",
    .types = "bool byte char decimal double dynamic float int long nint nuint object sbyte short string uint ulong "
             "ushort void",
    .constants = "true false null value",
};

constexpr Lexicon java_lex{
    .line_comments = "//", .block_comments = "/* */", .quotes = "\"'", .triple_quotes = true,
    .at_annotations = true, .capitalized_types = true, .upper_constants = true,
    .keywords = "abstract assert break case catch class const continue default do else enum exports extends final "
                "finally for goto if implements import instanceof interface module native new non-sealed open opens "
                "package permits private protected provides public record requires return sealed static strictfp "
                "super switch synchronized this throw throws to transient transitive try uses var void volatile while "
                "with yield",
    .types = "boolean byte char double float int long short",
    .constants = "true false null",
};

constexpr Lexicon kotlin_lex{
    .line_comments = "//", .block_comments = "/* */", .nested_comments = true, .quotes = "\"'",
    .triple_quotes = true, .at_annotations = true, .dollar_variables = true, .capitalized_types = true,
    .upper_constants = true,
    .keywords = "abstract actual annotation as break by catch class companion const constructor continue crossinline "
                "data do dynamic else enum expect external final finally for fun get if import in infix init inline "
                "inner interface internal is lateinit noinline object open operator out override package private "
                "protected public reified return sealed set super suspend tailrec this throw try typealias typeof val "
                "var vararg when where while",
    .types = "Any Boolean Byte Char Double Float Int Long Nothing Short String Unit Array List Map Set",
    .constants = "true false null",
};

constexpr Lexicon scala_lex{
    .line_comments = "//", .block_comments = "/* */", .nested_comments = true, .quotes = "\"'",
    .triple_quotes = true, .at_annotations = true, .capitalized_types = true,
    .keywords = "abstract case catch class def do else enum export extends extension final finally for forSome given "
                "if implicit import inline lazy match new object override package private protected return sealed "
                "super then this throw trait try type using val var while with yield",
    .constants = "true false null",
};

constexpr Lexicon groovy_lex{
    .line_comments = "//", .block_comments = "/* */", .quotes = "\"'", .triple_quotes = true,
    .at_annotations = true, .dollar_variables = true, .capitalized_types = true,
    .keywords = "abstract as assert break case catch class const continue def default do else enum extends final "
                "finally for goto if implements import in instanceof interface native new package private protected "
                "public return static super switch synchronized this threadsafe throw throws trait transient try var "
                "void volatile while",
    .types = "boolean byte char double float int long short",
    .constants = "true false null",
    .builtins = "println print plugins dependencies repositories implementation testImplementation api task apply",
};

constexpr Lexicon dart_lex{
    .line_comments = "//", .block_comments = "/* */", .nested_comments = true, .quotes = "\"'",
    .triple_quotes = true, .at_annotations = true, .dollar_variables = true, .capitalized_types = true,
    .keywords = "abstract as assert async await base break case catch class const continue covariant default deferred "
                "do dynamic else enum export extends extension external factory final finally for get hide if "
                "implements import in interface is late library mixin new on operator part required rethrow return "
                "sealed set show static super switch sync this throw try typedef var void when while with yield",
    .types = "int double num bool String List Map Set Object dynamic Future Stream",
    .constants = "true false null",
};

constexpr Lexicon swift_lex{
    .line_comments = "//", .block_comments = "/* */", .nested_comments = true, .quotes = "\"",
    .triple_quotes = true, .at_annotations = true, .capitalized_types = true,
    .keywords = "actor any associatedtype async await break case catch class continue convenience default defer deinit "
                "didSet do dynamic else enum extension fallthrough fileprivate final for func get guard if import "
                "indirect init inout internal is lazy let mutating nonisolated nonmutating open operator optional "
                "override private protocol public repeat required rethrows return set some static struct subscript "
                "super switch throw throws try typealias unowned var weak where while willSet",
    .constants = "true false nil self Self",
};

constexpr Lexicon go_lex{
    .line_comments = "//", .block_comments = "/* */", .quotes = "\"'`", .multiline_strings = false,
    .keywords = "break case chan const continue default defer else fallthrough for func go goto if import interface map "
                "package range return select struct switch type var",
    .types = "bool byte complex64 complex128 error float32 float64 int int8 int16 int32 int64 rune string uint uint8 "
             "uint16 uint32 uint64 uintptr any comparable",
    .constants = "true false nil iota",
    .builtins = "append cap clear close complex copy delete imag len make max min new panic print println real recover",
};

constexpr Lexicon rust_lex{
    .line_comments = "//", .block_comments = "/* */", .nested_comments = true, .quotes = "\"",
    .multiline_strings = true, .rust_raw_strings = true, .char_literals = true, .capitalized_types = true,
    .upper_constants = true,
    .keywords = "as async await break const continue crate dyn else enum extern fn for if impl in let loop match mod "
                "move mut pub ref return self static struct super trait type union unsafe use where while yield macro_rules",
    .types = "bool char f32 f64 i8 i16 i32 i64 i128 isize str u8 u16 u32 u64 u128 usize Self String Vec Option Result Box",
    .constants = "true false None Some Ok Err",
};

constexpr Lexicon zig_lex{
    .line_comments = "//", .quotes = "\"'", .identifier_extra = "@",
    .keywords = "addrspace align allowzero and anyframe anytype asm async await break callconv catch comptime const "
                "continue defer else enum errdefer error export extern fn for if inline linksection noalias noinline "
                "nosuspend opaque or orelse packed pub resume return struct suspend switch test threadlocal try union "
                "unreachable usingnamespace var volatile while",
    .types = "bool f16 f32 f64 f80 f128 i8 i16 i32 i64 i128 isize u8 u16 u32 u64 u128 usize void noreturn type anyerror "
             "comptime_int comptime_float c_int c_uint c_long c_char",
    .constants = "true false null undefined",
};

constexpr Lexicon d_lex{
    .line_comments = "//", .block_comments = "/* */ /+ +/", .nested_comments = true, .quotes = "\"'`",
    .multiline_strings = true, .at_annotations = true, .capitalized_types = true,
    .keywords = "abstract alias align asm assert auto body break case cast catch class const continue debug default "
                "delegate delete deprecated do else enum export extern final finally for foreach foreach_reverse "
                "function goto if immutable import in inout interface invariant is lazy macro mixin module new "
                "nothrow out override package pragma private protected public pure ref return scope shared static "
                "struct super switch synchronized template this throw try typeid typeof union unittest version while with",
    .types = "bool byte ubyte short ushort int uint long ulong cent ucent char wchar dchar float double real void string",
    .constants = "true false null",
};

constexpr Lexicon nim_lex{
    .line_comments = "#", .block_comments = "#[ ]#", .nested_comments = true, .quotes = "\"'",
    .triple_quotes = true,
    .keywords = "addr and as asm bind block break case cast concept const continue converter defer discard distinct "
                "div do elif else end enum except export finally for from func if import in include interface is "
                "isnot iterator let macro method mixin mod nil not notin object of or out proc ptr raise ref return "
                "shl shr static template try tuple type using var when while xor yield",
    .types = "int int8 int16 int32 int64 uint uint8 uint16 uint32 uint64 float float32 float64 bool char string seq "
             "array set",
    .constants = "true false nil",
};

constexpr Lexicon crystal_lex{
    .line_comments = "#", .quotes = "\"'", .multiline_strings = true, .at_annotations = true,
    .capitalized_types = true, .identifier_extra = "?!",
    .keywords = "abstract alias annotation as asm begin break case class def do else elsif end ensure enum extend for "
                "fun if in include instance_sizeof is_a? lib macro module next of out pointerof private protected "
                "require rescue return select self sizeof struct super then type typeof union unless until verbatim "
                "when while with yield",
    .constants = "true false nil",
};

constexpr Lexicon julia_lex{
    .line_comments = "#", .block_comments = "#= =#", .nested_comments = true, .quotes = "\"",
    .multiline_strings = true, .triple_quotes = true, .at_annotations = true, .dollar_variables = true,
    .capitalized_types = true, .identifier_extra = "!",
    .keywords = "abstract baremodule begin break catch const continue do else elseif end export finally for function "
                "global if import in isa let local macro module mutable primitive quote return struct try type using "
                "where while",
    .constants = "true false nothing missing NaN Inf pi",
    .builtins = "println print length push! pop! map filter reduce collect zeros ones",
};

// ---------------------------------------------------------------------------- scripting

constexpr Lexicon python_lex{
    .line_comments = "#", .quotes = "\"'", .triple_quotes = true, .at_annotations = true,
    .capitalized_types = false, .upper_constants = true,
    .keywords = "and as assert async await break case class continue def del elif else except finally for from "
                "global if import in is lambda match nonlocal not or pass raise return try type while with yield",
    .types = "int float complex str bytes bytearray bool list tuple dict set frozenset object type",
    .constants = "True False None self cls NotImplemented Ellipsis __name__ __file__",
    .builtins = "print len range enumerate zip map filter sorted reversed sum min max abs open isinstance super "
                "getattr setattr hasattr repr input iter next",
};

constexpr Lexicon ruby_lex{
    .line_comments = "#", .block_comments = "=begin =end", .quotes = "\"'`", .multiline_strings = true,
    .capitalized_types = true, .identifier_extra = "?!@",
    .keywords = "BEGIN END alias and begin break case class def defined? do else elsif end ensure for if in module "
                "next not or redo rescue retry return self super then undef unless until when while yield "
                "require require_relative include extend attr_accessor attr_reader attr_writer private protected "
                "public lambda proc",
    .constants = "true false nil __FILE__ __LINE__ __dir__",
    .builtins = "puts print p raise",
};

constexpr Lexicon php_lex{
    .line_comments = "// #", .block_comments = "/* */", .quotes = "\"'", .multiline_strings = true,
    .dollar_variables = true, .case_insensitive = true, .capitalized_types = false,
    .keywords = "abstract and array as break callable case catch class clone const continue declare default do echo "
                "else elseif empty enddeclare endfor endforeach endif endswitch endwhile enum eval exit extends final "
                "finally fn for foreach function global goto if implements include include_once instanceof insteadof "
                "interface isset list match namespace new or print private protected public readonly require "
                "require_once return static switch throw trait try unset use var while xor yield",
    .types = "int float bool string array object mixed void null iterable never self parent",
    .constants = "true false null __class__ __dir__ __file__ __function__ __line__ __method__ __namespace__",
};

constexpr Lexicon perl_lex{
    .line_comments = "#", .block_comments = "=pod =cut", .quotes = "\"'`", .multiline_strings = true,
    .dollar_variables = true,
    .keywords = "my our local sub package use no require if elsif else unless while until for foreach do last next "
                "redo return and or not eq ne lt gt le ge cmp x qw q qq m s tr y bless ref BEGIN END __END__ __DATA__",
    .builtins = "print printf say open close chomp chop push pop shift unshift split join keys values each defined "
                "exists delete die warn length substr sprintf map grep sort reverse",
};

constexpr Lexicon lua_lex{
    .line_comments = "--", .quotes = "\"'", .lua_long_brackets = true,
    .keywords = "and break do else elseif end for function goto if in local not or repeat return then until while",
    .constants = "true false nil self _G _VERSION",
    .builtins = "print pairs ipairs type tostring tonumber require error assert pcall setmetatable getmetatable "
                "select next rawget rawset unpack",
};

constexpr Lexicon r_lex{
    .line_comments = "#", .quotes = "\"'`", .multiline_strings = true, .identifier_extra = ".",
    .keywords = "if else repeat while function for in next break return switch library require",
    .constants = "TRUE FALSE NULL NA NA_integer_ NA_real_ NA_character_ Inf NaN T F",
    .builtins = "c list data.frame vector matrix print paste paste0 cat length sum mean apply lapply sapply",
};

constexpr std::string_view js_keywords =
    "as async await break case catch class const continue debugger default delete do else export extends finally for "
    "from function get if import in instanceof let new of return set static super switch this throw try typeof var "
    "void while with yield";
constexpr Lexicon js_lex{
    .line_comments = "//", .block_comments = "/* */", .quotes = "\"'", .template_literals = true,
    .capitalized_types = true, .upper_constants = true, .identifier_extra = "$",
    .keywords = js_keywords,
    .constants = "true false null undefined NaN Infinity globalThis",
    .builtins = "console require module exports window document",
};
constexpr Lexicon ts_lex{
    .line_comments = "//", .block_comments = "/* */", .quotes = "\"'", .template_literals = true,
    .at_annotations = true, .capitalized_types = true, .upper_constants = true, .identifier_extra = "$",
    .keywords = "abstract as asserts async await break case catch class const constructor continue debugger declare "
                "default delete do else enum export extends finally for from function get if implements import in "
                "infer instanceof interface is keyof let module namespace new of override private protected public "
                "readonly return satisfies set static super switch this throw try type typeof unique var void while "
                "with yield",
    .types = "any bigint boolean never number object string symbol unknown void",
    .constants = "true false null undefined NaN Infinity",
    .builtins = "console require",
};

constexpr Lexicon coffee_lex{
    .line_comments = "#", .block_comments = "### ###", .quotes = "\"'`", .multiline_strings = true,
    .triple_quotes = true, .at_annotations = true, .identifier_extra = "$",
    .keywords = "and break by catch class continue delete do else extends finally for if in instanceof is isnt loop new "
                "not of or own return super switch then this throw try typeof unless until when while yield",
    .constants = "true false null undefined yes no on off",
};

// ---------------------------------------------------------------------------- styles

constexpr Lexicon css_lex{
    .block_comments = "/* */", .quotes = "\"'", .at_annotations = true, .css = true,
    .keywords = "!important important and not only from to inherit initial unset revert auto none",
    .builtins = "url calc var rgb rgba hsl hsla oklch oklab lab lch color-mix min max clamp attr env linear-gradient "
                "radial-gradient translate rotate scale",
};
constexpr Lexicon scss_lex{
    .line_comments = "//", .block_comments = "/* */", .quotes = "\"'", .at_annotations = true,
    .dollar_variables = true, .css = true,
    .keywords = "!important !default and not or from to through if else each for while in",
    .builtins = "url calc var rgb rgba hsl hsla darken lighten mix map-get",
};
constexpr Lexicon less_lex{
    .line_comments = "//", .block_comments = "/* */", .quotes = "\"'", .at_annotations = true, .css = true,
    .keywords = "!important when and not",
    .builtins = "url calc rgb rgba hsl darken lighten fade",
};

// ---------------------------------------------------------------------------- shells

constexpr std::string_view sh_keywords =
    "if then else elif fi case esac for select while until do done in function time coproc return exit break "
    "continue export local readonly declare typeset unset shift source alias trap eval exec set";
constexpr Lexicon sh_lex{
    .line_comments = "#", .quotes = "\"'`", .multiline_strings = true, .dollar_variables = true,
    .identifier_extra = "-", .keywords = sh_keywords,
    .constants = "true false",
    .builtins = "echo printf cd pwd read test cat grep sed awk ls rm cp mv mkdir chmod chown find xargs sort uniq "
                "head tail wc tr cut",
};
constexpr Lexicon fish_lex{
    .line_comments = "#", .quotes = "\"'", .multiline_strings = true, .dollar_variables = true,
    .keywords = "and begin break builtin case command continue else end for function if in not or return set switch "
                "while argparse status",
    .constants = "true false",
    .builtins = "echo printf cd set_color string math test contains",
};
constexpr Lexicon powershell_lex{
    .line_comments = "#", .block_comments = "<# #>", .quotes = "\"'", .escape = '`', .multiline_strings = true,
    .dollar_variables = true, .case_insensitive = true, .identifier_extra = "-",
    .keywords = "begin break catch class continue data define do dynamicparam else elseif end enum exit filter finally "
                "for foreach from function hidden if in param process return static switch throw trap try until using "
                "var while -and -or -not -eq -ne -gt -ge -lt -le -like -notlike -match -notmatch -contains -in",
    .constants = "$true $false $null",
    .builtins = "write-host write-output get-childitem get-content set-content get-item new-item remove-item "
                "invoke-webrequest select-object where-object foreach-object",
};
constexpr Lexicon batch_lex{
    .line_comments = "^rem ^::", .quotes = "\"", .escape = '^', .case_insensitive = true,
    .keywords = "call cd chdir cls copy del dir echo else endlocal erase exist exit for goto if in md mkdir move not "
                "off on pause popd pushd rd ren rename rmdir set setlocal shift start title type do defined errorlevel "
                "enabledelayedexpansion",
};

// ---------------------------------------------------------------------------- query and schema languages

constexpr Lexicon sql_lex{
    .line_comments = "-- #", .block_comments = "/* */", .quotes = "'\"`", .escape = '\0', .multiline_strings = true,
    .case_insensitive = true,
    .keywords = "add all alter and any as asc begin between by case cascade check column commit constraint create "
                "cross database default delete desc distinct drop else end except exec exists explain foreign from "
                "full function grant group having if in index inner insert intersect into is join key left like limit "
                "merge natural not null offset on or order outer over partition primary procedure references replace "
                "returning revoke right rollback select set table then to top transaction trigger truncate union "
                "unique update using values view when where window with recursive",
    .types = "bigint binary bit blob boolean bool char date datetime decimal double float int integer interval json "
             "jsonb money nchar numeric nvarchar real serial smallint text time timestamp tinyint uuid varbinary varchar",
    .constants = "true false null current_date current_time current_timestamp",
    .builtins = "count sum avg min max coalesce nullif cast convert lower upper substring trim length now",
};
constexpr Lexicon graphql_lex{
    .line_comments = "#", .quotes = "\"", .triple_quotes = true, .at_annotations = true, .dollar_variables = true,
    .capitalized_types = true,
    .keywords = "query mutation subscription fragment on type interface union enum input scalar schema extend "
                "implements directive repeatable",
    .types = "Int Float String Boolean ID",
    .constants = "true false null",
};
constexpr Lexicon proto_lex{
    .line_comments = "//", .block_comments = "/* */", .quotes = "\"'", .capitalized_types = true,
    .keywords = "syntax edition package import public weak option message enum service rpc returns stream oneof map "
                "reserved extensions extend to max repeated optional required group",
    .types = "double float int32 int64 uint32 uint64 sint32 sint64 fixed32 fixed64 sfixed32 sfixed64 bool string bytes",
    .constants = "true false",
};
constexpr Lexicon thrift_lex{
    .line_comments = "// #", .block_comments = "/* */", .quotes = "\"'", .capitalized_types = true,
    .keywords = "namespace include cpp_include typedef enum struct union exception service extends throws oneway "
                "required optional const",
    .types = "bool byte i8 i16 i32 i64 double string binary list set map void",
    .constants = "true false",
};

// ---------------------------------------------------------------------------- build and config

constexpr Lexicon dockerfile_lex{
    .line_comments = "^#", .quotes = "\"'", .multiline_strings = false, .dollar_variables = true,
    .case_insensitive = true,
    .keywords = "from as run cmd label maintainer expose env add copy entrypoint volume user workdir arg onbuild "
                "stopsignal healthcheck shell",
};
constexpr Lexicon make_lex{
    .line_comments = "#", .quotes = "\"'", .dollar_variables = true,
    .keywords = "include sinclude -include override export unexport define endef ifeq ifneq ifdef ifndef else endif "
                "vpath .PHONY .SUFFIXES .DEFAULT .PRECIOUS .INTERMEDIATE .SECONDARY .DELETE_ON_ERROR",
    .builtins = "subst patsubst strip findstring filter filter-out sort word words wordlist firstword lastword dir "
                "notdir suffix basename addsuffix addprefix join wildcard realpath abspath error warning info shell "
                "origin flavor foreach if or and call eval file value",
};
constexpr Lexicon cmake_lex{
    .line_comments = "#", .block_comments = "#[[ ]]", .quotes = "\"", .multiline_strings = true,
    .dollar_variables = true, .case_insensitive = true,
    .keywords = "if elseif else endif foreach endforeach while endwhile function endfunction macro endmacro return "
                "break continue block endblock and or not defined strequal equal less greater matches",
    .constants = "on off true false yes no",
    .builtins = "add_executable add_library add_subdirectory add_test cmake_minimum_required project set unset "
                "option message include find_package target_link_libraries target_include_directories "
                "target_compile_definitions target_compile_options target_sources install list string file "
                "configure_file",
};
constexpr Lexicon hcl_lex{
    .line_comments = "# //", .block_comments = "/* */", .quotes = "\"", .dollar_variables = true,
    .keywords = "resource data variable output locals module provider terraform for in if each count depends_on "
                "for_each lifecycle dynamic backend required_providers",
    .constants = "true false null",
};
constexpr Lexicon nix_lex{
    .line_comments = "#", .block_comments = "/* */", .quotes = "\"", .multiline_strings = true,
    .dollar_variables = true, .identifier_extra = "-'",
    .keywords = "let in with rec inherit if then else assert or import",
    .constants = "true false null",
    .builtins = "builtins derivation fetchurl fetchTarball mkDerivation callPackage",
};

// ---------------------------------------------------------------------------- hardware and low level

constexpr Lexicon asm_lex{
    .line_comments = "; # //", .block_comments = "/* */", .quotes = "\"'", .case_insensitive = true,
    .identifier_extra = ".$%",
    .keywords = "mov movzx movsx lea add sub mul imul div idiv inc dec neg and or xor not shl shr sal sar rol ror cmp "
                "test jmp je jne jz jnz jg jge jl jle ja jae jb jbe call ret push pop nop int syscall leave enter "
                "ldr str ldm stm b bl bx blx beq bne cbz cbnz adr adrp addi li la lw sw jal jalr "
                "section segment global extern db dw dd dq resb resw resd resq equ times align org bits "
                ".text .data .bss .section .globl .global .byte .word .long .quad .ascii .asciz .string .align .equ",
    .types = "byte word dword qword ptr",
    .constants = "rax rbx rcx rdx rsi rdi rbp rsp r8 r9 r10 r11 r12 r13 r14 r15 eax ebx ecx edx esi edi ebp esp ax "
                 "bx cx dx al bl cl dl ah bh ch dh sp fp lr pc x0 x1 x2 x3 w0 w1",
};
constexpr Lexicon verilog_lex{
    .line_comments = "//", .block_comments = "/* */", .quotes = "\"", .hash_preprocessor = false,
    .dollar_variables = true, .identifier_extra = "`",
    .keywords = "always always_comb always_ff always_latch and assign automatic begin case casex casez class "
                "default defparam do else end endcase endclass endfunction endgenerate endinterface endmodule "
                "endpackage endprogram endtask enum for forever fork function generate genvar if import initial inout "
                "input interface join localparam module negedge or output package parameter posedge program repeat "
                "return struct task typedef unique while wire reg logic integer real time bit byte int longint",
    .constants = "1'b0 1'b1",
};
constexpr Lexicon vhdl_lex{
    .line_comments = "--", .block_comments = "/* */", .quotes = "\"", .escape = '\0', .case_insensitive = true,
    .keywords = "abs access after alias all and architecture array assert attribute begin block body buffer bus case "
                "component configuration constant disconnect downto else elsif end entity exit file for function "
                "generate generic group guarded if impure in inertial inout is label library linkage literal loop "
                "map mod nand new next nor not null of on open or others out package port postponed procedure "
                "process pure range record register reject rem report return rol ror select severity signal shared "
                "sla sll sra srl subtype then to transport type unaffected units until use variable wait when while "
                "with xnor xor",
    .types = "std_logic std_logic_vector std_ulogic signed unsigned integer natural positive boolean bit bit_vector "
             "string real time character",
    .constants = "true false",
};

// ---------------------------------------------------------------------------- classic and functional

constexpr Lexicon fortran_lex{
    .line_comments = "!", .quotes = "\"'", .escape = '\0', .case_insensitive = true,
    .keywords = "allocatable allocate assign associate block call case character class close common complex contains "
                "continue cycle data deallocate default dimension do double else elseif elsewhere end enddo endif "
                "entry enum equivalence exit external forall format function go goto if implicit in include inout "
                "integer intent interface intrinsic kind logical module namelist none nullify only open optional out "
                "parameter pointer precision print private procedure program public pure read real recursive result "
                "return save select sequence stop subroutine target then to type use where while write",
    .constants = ".true. .false.",
};
constexpr Lexicon pascal_lex{
    .line_comments = "//", .block_comments = "{ } (* *)", .quotes = "'", .escape = '\0', .case_insensitive = true,
    .keywords = "and array as asm begin case class const constructor destructor div do downto else end except exports "
                "file finalization finally for function goto if implementation in inherited initialization inline "
                "interface is label library mod nil not object of or out packed procedure program property raise "
                "record repeat resourcestring set shl shr string then threadvar to try type unit until uses var while "
                "with xor private protected public published override virtual",
    .types = "integer cardinal shortint smallint longint int64 byte word longword real single double extended "
             "currency boolean char ansichar widechar ansistring widestring pointer",
    .constants = "true false nil",
};
constexpr Lexicon ada_lex{
    .line_comments = "--", .quotes = "\"", .escape = '\0', .case_insensitive = true,
    .keywords = "abort abs abstract accept access aliased all and array at begin body case constant declare delay "
                "delta digits do else elsif end entry exception exit for function generic goto if in interface is "
                "limited loop mod new not null of or others out overriding package pragma private procedure "
                "protected raise range record rem renames requeue return reverse select separate some subtype "
                "synchronized tagged task terminate then type until use when while with xor",
    .types = "integer natural positive float boolean character string duration",
    .constants = "true false",
};
constexpr Lexicon cobol_lex{
    .line_comments = "*> ^*", .quotes = "\"'", .escape = '\0', .case_insensitive = true, .identifier_extra = "-",
    .keywords = "identification division program-id environment configuration input-output file-control data "
                "working-storage linkage procedure section select assign to fd pic picture value move add subtract "
                "multiply divide compute display accept perform until varying if else end-if evaluate when end-evaluate "
                "stop run goback call using giving copy redefines occurs times open close read write rewrite at end "
                "not exit initialize string unstring inspect replacing",
    .constants = "zero zeros zeroes space spaces high-value high-values low-value low-values quote quotes",
};
constexpr Lexicon haskell_lex{
    .line_comments = "--", .block_comments = "{- -}", .nested_comments = true, .quotes = "\"",
    .char_literals = true, .capitalized_types = true, .identifier_extra = "'",
    .keywords = "case class data default deriving do else family forall foreign hiding if import in infix infixl "
                "infixr instance let mdo module newtype of proc qualified rec then type where",
    .constants = "True False Nothing Just Left Right",
    .builtins = "map filter foldr foldl putStrLn print show read return pure fmap",
};
constexpr Lexicon ocaml_lex{
    .block_comments = "(* *)", .nested_comments = true, .quotes = "\"", .char_literals = true,
    .capitalized_types = true, .identifier_extra = "'",
    .keywords = "and as assert begin class constraint do done downto else end exception external for fun function "
                "functor if in include inherit initializer lazy let match method module mutable new nonrec object of "
                "open or private rec sig struct then to try type val virtual when while with",
    .types = "int float bool char string unit list array option ref",
    .constants = "true false None Some",
};
constexpr Lexicon fsharp_lex{
    .line_comments = "//", .block_comments = "(* *)", .nested_comments = true, .quotes = "\"",
    .triple_quotes = true, .char_literals = true, .hash_preprocessor = true, .capitalized_types = true,
    .keywords = "abstract and as assert base begin class default delegate do done downcast downto elif else end "
                "exception extern for fun function global if in inherit inline interface internal lazy let match "
                "member module mutable namespace new of open or override private public rec return static struct "
                "then to try type upcast use val void when while with yield",
    .types = "int float string bool char unit list array seq option decimal byte int64",
    .constants = "true false null None Some",
};
constexpr Lexicon clojure_lex{
    .line_comments = ";", .quotes = "\"", .multiline_strings = true, .lisp = true,
    .keywords = "def defn defn- defmacro defmulti defmethod defprotocol defrecord deftype fn let if if-not when "
                "when-not cond case do loop recur try catch finally throw ns require import quote var and or not",
    .constants = "true false nil",
};
constexpr Lexicon lisp_lex{
    .line_comments = ";", .block_comments = "#| |#", .nested_comments = true, .quotes = "\"",
    .multiline_strings = true, .lisp = true,
    .keywords = "defun defmacro defvar defparameter defconstant defclass defmethod defgeneric defstruct lambda let "
                "let* flet labels if when unless cond case progn prog1 loop do dolist dotimes setq setf return "
                "return-from block and or not quote function",
    .constants = "t nil",
};
constexpr Lexicon scheme_lex{
    .line_comments = ";", .block_comments = "#| |#", .nested_comments = true, .quotes = "\"",
    .multiline_strings = true, .lisp = true,
    .keywords = "define define-syntax define-record-type lambda let let* letrec letrec* if cond case when unless "
                "begin do and or not set! quote quasiquote unquote syntax-rules import library export",
    .constants = "#t #f #true #false",
};
constexpr Lexicon erlang_lex{
    .line_comments = "%", .quotes = "\"", .char_literals = false, .capitalized_types = false,
    .keywords = "after and andalso band begin bnot bor bsl bsr bxor case catch cond div end fun if let not of or "
                "orelse receive rem try when xor -module -export -import -define -record -include -behaviour -spec "
                "-type",
    .constants = "true false undefined ok error",
};
constexpr Lexicon elixir_lex{
    .line_comments = "#", .quotes = "\"'", .multiline_strings = true, .triple_quotes = true, .at_annotations = true,
    .capitalized_types = true, .identifier_extra = "?!",
    .keywords = "after alias and case catch cond def defmacro defmodule defp defprotocol defimpl defstruct do else end "
                "fn for if import in not or quote raise receive require rescue try unless unquote use when with",
    .constants = "true false nil",
};
constexpr Lexicon tcl_lex{
    .line_comments = "#", .quotes = "\"", .multiline_strings = true, .dollar_variables = true,
    .keywords = "after append array break catch close concat continue dict else elseif eof error eval exec exit expr "
                "for foreach format gets global if incr info lappend lindex list llength lsort namespace open package "
                "proc puts regexp regsub rename return set split string switch then uplevel upvar variable while",
};
constexpr Lexicon awk_lex{
    .line_comments = "#", .quotes = "\"", .dollar_variables = true,
    .keywords = "BEGIN END BEGINFILE ENDFILE if else while for do break continue next nextfile exit return delete in "
                "function func getline print printf",
    .constants = "NR NF FS OFS RS ORS FILENAME FNR",
    .builtins = "length substr index split sub gsub match sprintf tolower toupper system close",
};
constexpr Lexicon vim_lex{
    .line_comments = "^\"", .quotes = "'", .escape = '\0',
    .keywords = "if else elseif endif for endfor while endwhile function endfunction return let unlet call execute "
                "set setlocal map nnoremap inoremap vnoremap noremap autocmd augroup command syntax highlight",
};
constexpr Lexicon vb_lex{
    .line_comments = "' ^rem", .quotes = "\"", .escape = '\0', .case_insensitive = true,
    .keywords = "addhandler addressof alias and andalso as byref byval call case catch class const continue declare "
                "default delegate dim do each else elseif end enum erase error event exit finally for friend function "
                "get global goto handles if implements imports in inherits interface is isnot let lib like loop me "
                "mod module mustinherit mustoverride mybase namespace new next not of on option optional or orelse "
                "overloads overridable overrides paramarray partial private property protected public raiseevent "
                "readonly redim removehandler resume return select set shadows shared static step stop structure sub "
                "synclock then throw to try typeof using when while with withevents writeonly xor",
    .types = "boolean byte char date decimal double integer long object short single string",
    .constants = "true false nothing",
};
constexpr Lexicon solidity_lex{
    .line_comments = "//", .block_comments = "/* */", .quotes = "\"'", .capitalized_types = true,
    .keywords = "abstract anonymous as assembly break calldata catch constant constructor continue contract delete do "
                "else emit enum event external fallback for function if immutable import indexed interface internal "
                "is library mapping memory modifier new override payable pragma private public pure receive return "
                "returns revert require storage struct try type unchecked using view virtual while",
    .types = "address bool bytes bytes32 int int256 string uint uint8 uint256",
    .constants = "true false wei gwei ether",
};
constexpr Lexicon glsl_lex{
    .line_comments = "//", .block_comments = "/* */", .quotes = "\"", .hash_preprocessor = true,
    .upper_constants = true,
    .keywords = "attribute break case centroid const continue default discard do else flat for highp if in inout "
                "invariant layout lowp mediump noperspective out precision return smooth struct switch uniform "
                "varying while buffer shared coherent volatile restrict readonly writeonly",
    .types = "void bool int uint float double vec2 vec3 vec4 dvec2 dvec3 dvec4 bvec2 bvec3 bvec4 ivec2 ivec3 ivec4 "
             "uvec2 uvec3 uvec4 mat2 mat3 mat4 sampler2D sampler3D samplerCube",
    .constants = "true false gl_Position gl_FragCoord gl_FragColor",
};
constexpr Lexicon elm_lex{
    .line_comments = "--", .block_comments = "{- -}", .nested_comments = true, .quotes = "\"",
    .triple_quotes = true, .char_literals = true, .capitalized_types = true,
    .keywords = "as case else exposing if import in let module of port then type alias",
    .constants = "True False Nothing Just",
};

// ---------------------------------------------------------------------------- markup-based component formats

constexpr Lexicon vue_lex{
    .block_comments = "<!-- -->", .quotes = "\"'", .markup = true,
};

// ---------------------------------------------------------------------------- WebAssembly text

constexpr Lexicon wat_lex{
    .line_comments = ";;", .block_comments = "(; ;)", .nested_comments = true, .quotes = "\"", .dollar_variables = true,
    .lisp = true,
    .keywords = "module func param result local type import export memory table global elem data start mut block loop "
                "if then else end br br_if br_table return call call_indirect unreachable nop drop select offset align "
                "declare item",
    .types = "i32 i64 f32 f64 v128 funcref externref anyfunc",
};

// ---------------------------------------------------------------------------- diff

constexpr Lexicon diff_lex{.diff = true};

// ---------------------------------------------------------------------------- the table

constexpr std::array<Language, 92> language_table = {{
    {"c", "C", "c", "text/x-c", 55, "", "", "", &c_lex},
    {"h", "C header", "h", "text/x-c", 40, "", "", "", &cpp_lex},
    {"cpp", "C++", "cpp cc cxx c++ ino", "text/x-c++", 55, "", "", "c++ cplusplus", &cpp_lex},
    {"hpp", "C++ header", "hpp hh hxx h++ ipp tpp", "text/x-c++", 35, "", "", "", &cpp_lex},
    {"cu", "CUDA", "cu cuh", "text/x-cuda", 15, "", "", "cuda", &cpp_lex},
    {"cs", "C#", "cs csx", "text/x-csharp", 50, "", "", "csharp c#", &csharp_lex},
    {"java", "Java", "java", "text/x-java", 55, "", "", "", &java_lex},
    {"kt", "Kotlin", "kt kts", "text/x-kotlin", 40, "kotlin", "", "kotlin", &kotlin_lex},
    {"swift", "Swift", "swift", "text/x-swift", 40, "swift", "", "", &swift_lex},
    {"m", "Objective-C", "m", "text/x-objectivec", 25, "", "", "objc objective-c", &objc_lex},
    {"mm", "Objective-C++", "mm", "text/x-objectivec++", 15, "", "", "objcpp objective-c++", &objc_lex},
    {"go", "Go", "go", "text/x-go", 50, "", "", "golang", &go_lex},
    {"rs", "Rust", "rs", "text/x-rust", 50, "", "", "rust", &rust_lex},
    {"zig", "Zig", "zig zon", "text/x-zig", 15, "", "", "", &zig_lex},
    {"d", "D", "d di", "text/x-d", 10, "rdmd", "", "dlang", &d_lex},
    {"nim", "Nim", "nim nims nimble", "text/x-nim", 10, "", "", "", &nim_lex},
    {"cr", "Crystal", "cr", "text/x-crystal", 8, "crystal", "", "crystal", &crystal_lex},
    {"jl", "Julia", "jl", "text/x-julia", 20, "julia", "", "julia", &julia_lex},
    {"dart", "Dart", "dart", "text/x-dart", 25, "dart", "", "", &dart_lex},
    {"scala", "Scala", "scala sc", "text/x-scala", 25, "scala", "", "", &scala_lex},
    {"groovy", "Groovy", "groovy gvy", "text/x-groovy", 15, "groovy", "Jenkinsfile", "", &groovy_lex},
    {"gradle", "Gradle build script", "gradle", "text/x-groovy", 25, "", "", "", &groovy_lex},
    {"py", "Python", "py pyw pyi", "text/x-python", 65, "python python2 python3 pypy pypy3", "SConstruct SConscript",
     "python python3 py3", &python_lex},
    {"rb", "Ruby", "rb rake gemspec ru", "text/x-ruby", 40, "ruby jruby", "Rakefile Gemfile Vagrantfile Podfile",
     "ruby", &ruby_lex},
    {"php", "PHP", "php phtml php3 php4 php5 php7 phps", "application/x-php", 45, "php", "", "", &php_lex},
    {"pl", "Perl", "pl pm t pod", "text/x-perl", 25, "perl", "", "perl", &perl_lex},
    {"lua", "Lua", "lua", "text/x-lua", 30, "lua luajit", "", "", &lua_lex},
    {"r", "R", "r rmd", "text/x-r", 30, "rscript", ".Rprofile", "rlang", &r_lex},
    {"js", "JavaScript", "js cjs", "text/javascript", 70, "node nodejs deno bun", "", "javascript ecmascript", &js_lex},
    {"mjs", "JavaScript module", "mjs", "text/javascript", 30, "", "", "", &js_lex},
    {"jsx", "JavaScript JSX", "jsx", "text/jsx", 35, "", "", "", &js_lex},
    {"ts", "TypeScript", "ts mts cts", "text/x-typescript", 60, "ts-node tsx", "", "typescript", &ts_lex},
    {"tsx", "TypeScript JSX", "tsx", "text/tsx", 40, "", "", "", &ts_lex},
    {"coffee", "CoffeeScript", "coffee", "text/x-coffeescript", 8, "coffee", "Cakefile", "coffeescript", &coffee_lex},
    {"vue", "Vue single-file component", "vue", "text/x-vue", 30, "", "", "", &vue_lex},
    {"svelte", "Svelte component", "svelte", "text/x-svelte", 20, "", "", "", &vue_lex},
    {"css", "CSS stylesheet", "css", "text/css", 60, "", "", "", &css_lex},
    {"scss", "Sass (SCSS) stylesheet", "scss", "text/x-scss", 35, "", "", "", &scss_lex},
    {"sass", "Sass (indented) stylesheet", "sass", "text/x-sass", 15, "", "", "", &scss_lex},
    {"less", "Less stylesheet", "less", "text/x-less", 20, "", "", "", &less_lex},
    {"sh", "Shell script", "sh", "application/x-sh", 50, "sh dash ash ksh", "", "shell posix", &sh_lex},
    {"bash", "Bash script", "bash", "application/x-sh", 40, "bash", ".bashrc .bash_profile .bash_aliases .profile",
     "", &sh_lex},
    {"zsh", "Zsh script", "zsh", "application/x-sh", 25, "zsh", ".zshrc .zprofile .zshenv", "", &sh_lex},
    {"fish", "Fish script", "fish", "application/x-fish", 12, "fish", "", "", &fish_lex},
    {"ps1", "PowerShell script", "ps1 psm1 psd1", "text/x-powershell", 30, "pwsh powershell", "", "powershell",
     &powershell_lex},
    {"bat", "Windows batch file", "bat cmd", "application/x-bat", 30, "", "", "batch cmd", &batch_lex},
    {"sql", "SQL script", "sql ddl dml", "application/sql", 50, "", "", "", &sql_lex},
    {"graphql", "GraphQL", "graphql gql graphqls", "application/graphql", 25, "", "", "gql", &graphql_lex},
    {"proto", "Protocol Buffers", "proto", "text/x-protobuf", 25, "", "", "protobuf", &proto_lex},
    {"thrift", "Apache Thrift", "thrift", "text/x-thrift", 8, "", "", "", &thrift_lex},
    {"dockerfile", "Dockerfile", "dockerfile containerfile", "text/x-dockerfile", 40, "",
     "Dockerfile Containerfile dockerfile", "docker", &dockerfile_lex},
    {"makefile", "Makefile", "mk mak make", "text/x-makefile", 35, "make", "Makefile makefile GNUmakefile BSDmakefile",
     "make", &make_lex},
    {"cmake", "CMake script", "cmake", "text/x-cmake", 30, "", "CMakeLists.txt", "", &cmake_lex},
    {"tf", "Terraform (HCL)", "tf tfvars hcl", "text/x-terraform", 25, "", "", "terraform hcl", &hcl_lex},
    {"nix", "Nix expression", "nix", "text/x-nix", 12, "", "", "", &nix_lex},
    {"asm", "Assembly", "asm s nasm", "text/x-asm", 20, "", "", "assembly nasm gas", &asm_lex},
    {"v", "Verilog", "v vh", "text/x-verilog", 12, "", "", "verilog", &verilog_lex},
    {"sv", "SystemVerilog", "sv svh", "text/x-systemverilog", 10, "", "", "systemverilog", &verilog_lex},
    {"vhdl", "VHDL", "vhdl vhd", "text/x-vhdl", 10, "", "", "", &vhdl_lex},
    {"f90", "Fortran", "f90 f95 f03 f08 f for ftn", "text/x-fortran", 12, "", "", "fortran", &fortran_lex},
    {"pas", "Pascal", "pas pp dpr lpr", "text/x-pascal", 12, "", "", "pascal delphi", &pascal_lex},
    {"ada", "Ada", "ada adb ads", "text/x-ada", 8, "", "", "", &ada_lex},
    {"cob", "COBOL", "cob cbl cpy", "text/x-cobol", 8, "", "", "cobol", &cobol_lex},
    {"hs", "Haskell", "hs lhs", "text/x-haskell", 20, "runhaskell", "", "haskell", &haskell_lex},
    {"ml", "OCaml", "ml mli", "text/x-ocaml", 12, "ocaml", "", "ocaml", &ocaml_lex},
    {"fs", "F#", "fs fsi fsx", "text/x-fsharp", 12, "", "", "fsharp f#", &fsharp_lex},
    {"elm", "Elm", "elm", "text/x-elm", 8, "", "", "", &elm_lex},
    {"clj", "Clojure", "clj cljs cljc", "text/x-clojure", 15, "clojure bb", "", "clojure", &clojure_lex},
    {"lisp", "Common Lisp", "lisp lsp cl asd", "text/x-lisp", 10, "sbcl clisp", "", "commonlisp", &lisp_lex},
    {"el", "Emacs Lisp", "el", "text/x-elisp", 8, "emacs", ".emacs", "elisp emacs-lisp", &lisp_lex},
    {"scm", "Scheme", "scm ss sld", "text/x-scheme", 8, "guile scheme", "", "scheme", &scheme_lex},
    {"rkt", "Racket", "rkt", "text/x-racket", 6, "racket", "", "racket", &scheme_lex},
    {"erl", "Erlang", "erl hrl", "text/x-erlang", 10, "escript", "rebar.config", "erlang", &erlang_lex},
    {"ex", "Elixir", "ex exs", "text/x-elixir", 15, "elixir", "mix.exs", "elixir", &elixir_lex},
    {"tcl", "Tcl", "tcl tk", "text/x-tcl", 6, "tclsh wish", "", "", &tcl_lex},
    {"awk", "AWK", "awk", "text/x-awk", 8, "awk gawk mawk nawk", "", "", &awk_lex},
    {"vim", "Vim script", "vim", "text/x-vim", 8, "", ".vimrc _vimrc .gvimrc", "vimscript viml", &vim_lex},
    {"vb", "Visual Basic", "vb vbs bas", "text/x-vb", 15, "", "", "vbnet vbscript", &vb_lex},
    {"sol", "Solidity", "sol", "text/x-solidity", 10, "", "", "solidity", &solidity_lex},
    {"glsl", "GLSL shader", "glsl vert frag geom comp tesc tese", "text/x-glsl", 12, "", "", "shader", &glsl_lex},
    {"hlsl", "HLSL shader", "hlsl fx", "text/x-hlsl", 8, "", "", "", &glsl_lex},
    {"wgsl", "WGSL shader", "wgsl", "text/x-wgsl", 6, "", "", "", &rust_lex},
    {"gd", "GDScript", "gd", "text/x-gdscript", 8, "", "", "gdscript", &python_lex},
    {"pyx", "Cython", "pyx pxd", "text/x-cython", 6, "", "", "cython", &python_lex},
    {"bzl", "Starlark (Bazel)", "bzl star", "text/x-starlark", 10, "", "BUILD BUILD.bazel WORKSPACE MODULE.bazel",
     "starlark bazel", &python_lex},
    {"jsonnet", "Jsonnet", "jsonnet libsonnet", "text/x-jsonnet", 5, "", "", "", &js_lex},
    {"hx", "Haxe", "hx", "text/x-haxe", 5, "", "", "haxe", &java_lex},
    {"apex", "Apex", "cls trigger apex", "text/x-apex", 5, "", "", "", &java_lex},
    {"qml", "QML", "qml", "text/x-qml", 6, "", "", "", &js_lex},
    {"pwn", "Pawn", "pwn", "text/x-pawn", 3, "", "", "", &c_lex},
    {"wat", "WebAssembly text", "wat wast", "text/x-wat", 20, "", "", "webassembly-text", &wat_lex},
    {"diff", "Diff", "diff", "text/x-diff", 30, "", "", "patch udiff", &diff_lex},
}};

}  // namespace

std::span<const Language> languages() noexcept { return language_table; }

const Language* find_language(std::string_view name) noexcept {
  const std::string_view wanted = util::trim(name);
  if (wanted.empty()) return nullptr;
  const auto in_list = [&wanted](std::string_view list) {
    std::size_t start = 0;
    while (start < list.size()) {
      std::size_t end = list.find(' ', start);
      if (end == std::string_view::npos) end = list.size();
      if (util::iequals(list.substr(start, end - start), wanted)) return true;
      start = end + 1;
    }
    return false;
  };
  for (const Language& language : language_table) {
    if (util::iequals(language.id, wanted) || util::iequals(language.name, wanted)) return &language;
  }
  for (const Language& language : language_table) {
    if (in_list(language.aliases) || in_list(language.extensions)) return &language;
  }
  // Common names of formats owned by other families that share a lexicon.
  if (util::iequals(wanted, "patch")) return find_language("diff");
  return nullptr;
}

}  // namespace cvt::code
