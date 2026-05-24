# Editor support

## Vim / Neovim

Drop the two files in `vim/` into the matching slots in your config:

```
cp -r editor/vim/syntax    ~/.vim/
cp -r editor/vim/ftdetect  ~/.vim/
```

Or for Neovim:

```
cp -r editor/vim/syntax    ~/.config/nvim/
cp -r editor/vim/ftdetect  ~/.config/nvim/
```

Or symlink, so updates flow automatically:

```
ln -s "$(pwd)/editor/vim/syntax/knot.vim"   ~/.vim/syntax/
ln -s "$(pwd)/editor/vim/ftdetect/knot.vim" ~/.vim/ftdetect/
```

Open any `*.knot` file. You should see comments, strings, numbers,
hard keywords (`if`, `for`, `def`, `return`, `test`, `show`,
`narrate`, `iterate`, `repeat`, ...), the soft keywords (`take`,
`fn`), atoms (`True`, `False`, `None`), builtins (`sin`, `print`,
`linspace`, ...), and stdlib functions (`solve`, `simpson`,
`power_iter`, ...) all highlighted in the colors your colorscheme
assigns to `Keyword`, `Function`, `String`, `Number`, `Boolean`,
`Constant`, `Comment`, `Todo`, and `Operator`.

## Tree-sitter

Not yet. The plan calls for a real grammar in v0.2+. The vim file
above is regex-based and gets the easy 95% of highlighting right.

## Other editors

The CodeMirror grammar embedded in `web/index.html` (search for
`CodeMirror.defineSimpleMode('knot', ...)`) is also regex-based
and could be ported to any editor with a similar pattern model.
