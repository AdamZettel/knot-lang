" Vim syntax file for the knot language.
"
" Drop both files into ~/.vim/ (or ~/.config/nvim/):
"   editor/vim/syntax/knot.vim    -> ~/.vim/syntax/knot.vim
"   editor/vim/ftdetect/knot.vim  -> ~/.vim/ftdetect/knot.vim
" or symlink. Neovim picks them up from ~/.config/nvim/{syntax,ftdetect}/.
"
" The keyword and builtin lists are kept in sync with the
" src/token.hpp keyword map, the interpreter's register_builtins(),
" and the stdlib/stdlib.knot top-level defs. Adding a keyword or
" builtin to the language? Update this file too.

if exists("b:current_syntax")
  finish
endif

" ---- Comments -------------------------------------------------------

syn match knotComment "#.*$" contains=knotTodo
syn keyword knotTodo TODO FIXME XXX NOTE contained

" ---- Strings --------------------------------------------------------

syn region knotString start=+"+ skip=+\\\\\|\\"+ end=+"+

" ---- Numbers --------------------------------------------------------

syn match knotNumber "\<\d\+\>"
syn match knotNumber "\<\d\+\.\d*\>"
syn match knotNumber "\<\.\d\+\>"
syn match knotNumber "\<\d\+\([eE][+-]\=\d\+\)\>"
syn match knotNumber "\<\d\+\.\d*\([eE][+-]\=\d\+\)\>"
syn match knotNumber "\<\d\+\.\d*\([eE][+-]\=\d\+\)\=\>"

" ---- Hard keywords (src/token.hpp keyword map) ----------------------

syn keyword knotKeyword if else while loop as for to in def return
syn keyword knotKeyword break continue test show narrate iterate repeat
syn keyword knotKeyword and or not

" ---- Soft keywords --------------------------------------------------
" Both `take` (phrase introducer) and `fn` (anonymous-function
" introducer) are technically still allowed as plain identifiers, but
" the overwhelmingly common case is the keyword sense. Coloring them
" as keywords matches reader intuition.

syn keyword knotKeyword take fn

" ---- Atoms ----------------------------------------------------------

syn keyword knotBoolean True False
syn keyword knotConstant None

" ---- Registry / declaration keywords (Phase 7+; reserved) -----------
" Listed but not yet used in the parser. Highlighting them now is
" forward-compatible.

syn keyword knotKeyword solver method problem requires implements canonical

" ---- Builtin functions (src/interpreter.hpp register_builtins) ------

syn keyword knotBuiltin input print panic plot plot_save
syn keyword knotBuiltin at set append format
syn keyword knotBuiltin num str len rows cols
syn keyword knotBuiltin zeros ones eye dot norm matmul transpose
syn keyword knotBuiltin sqrt abs sin cos tan asin acos atan atan2
syn keyword knotBuiltin exp log pow floor ceil round
syn keyword knotBuiltin sort_vec
syn keyword knotBuiltin rng_seed rng_uniform rng_normal
syn keyword knotBuiltin read_csv write_csv

" ---- Stdlib top-level defs (stdlib/stdlib.knot) ---------------------

syn keyword knotStdlib sum prod mean vmin vmax argmin argmax
syn keyword knotStdlib linspace arange reverse fill copy_vec copy_mat
syn keyword knotStdlib variance std floor_div median sort
syn keyword knotStdlib trace diag lu solve power_iter
syn keyword knotStdlib normalize project orthogonalize angle dist
syn keyword knotStdlib numerical_derivative
syn keyword knotStdlib bisect newton newton_numeric
syn keyword knotStdlib trapezoid simpson rk4 golden_section
syn keyword knotStdlib assert assert_msg assert_eq assert_near assert_mat_near

" ---- Operators ------------------------------------------------------

syn match knotOperator "+\|-\|\*\|/\|%\|@"
syn match knotOperator "=\|==\|!=\|<\|<=\|>\|>="
syn match knotOperator "+=\|-=\|\*=\|/="
syn match knotOperator "&&\|||\|!"
syn match knotOperator "->"
syn match knotOperator "|>"

" ---- def NAME and function calls ------------------------------------
" Color the function name in a `def NAME(...)` declaration.

syn match knotFunction "\(def\s\+\)\@<=\h\w*"

" ---- Highlight links ------------------------------------------------

hi def link knotComment   Comment
hi def link knotTodo      Todo
hi def link knotString    String
hi def link knotNumber    Number
hi def link knotKeyword   Keyword
hi def link knotBoolean   Boolean
hi def link knotConstant  Constant
hi def link knotBuiltin   Function
hi def link knotStdlib    Function
hi def link knotOperator  Operator
hi def link knotFunction  Function

let b:current_syntax = "knot"
