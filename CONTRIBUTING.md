# Contributing to tree-sitter-scala

First off, thanks for being willing to contribute to making syntax highlighting
in Scala better! This document will hopefully help you get set up, understand
how to work on the codebase, or link to places to help you understand
tree-sitter.

## Requirements

- [tree-sitter CLI](https://github.com/tree-sitter/tree-sitter/tree/master/cli)
- Node.js version 18.0 or greater
- C Compiler

Refer to the [tree-sitter
documentation](https://tree-sitter.github.io/tree-sitter/creating-parsers/1-getting-started.html)
for more details and specifics.

If you use nix you can enter a nix-shell (`nix-shell .`) which will install them
for you.

## Getting Started

To get started with contributing to the grammar you'll first need to install the
project's dependencies:

```sh
npm install
```

The general flow will often start with adding a test case to `./test/corpus`. You can
find details on how testing works with tree-sitter
[here](https://tree-sitter.github.io/tree-sitter/creating-parsers/5-writing-tests.html).

Once you've added your test case you'll want to then make the required changes
to `grammar.js`, regenerate and recompile the parser, and run the tests:

```sh
tree-sitter generate
tree-sitter test
```

Then adjust as necessary. Note that depending on your change you may also have
to touch the `/src/scanner.c` file if you need more advanced features like
look-ahead.

## Pending Tests

In `./test/corpus`, there are several files named `*-pending.txt`. These contain tests labeled with `:skip`, meaning they are not currently run. Each test includes examples of valid Scala syntax that `tree-sitter-scala` is known to fail to parse. If you’d like to contribute to `tree-sitter-scala`, a good place to start is by trying to fix one of these tests.

## Syntax highlighting

Right now the most common use-case for syntax highlight using tree-sitter is
[nvim-treesitter](https://github.com/nvim-treesitter/nvim-treesitter), which
means much of our testing is in relation to it. You can find the syntax
highlighting tests in `test/highlight/*.scala`. You can read more about this
type of testing 
[here](https://tree-sitter.github.io/tree-sitter/3-syntax-highlighting.html#unit-testing). 
These test will be run automatically with `tree-sitter test`.

### tree-sitter highlight

Another way to test your syntax highlighting locally is to use the `tree-sitter
highlight` command. Note that you'll need to have `tree-sitter` installed
globally for this to work. Once you have it installed you'll want to follow the
instructions [here](https://tree-sitter.github.io/tree-sitter/3-syntax-highlighting.html#overview) 
to setup a local config that points towards this repo to be used as a parser. 
Once done you can then do the following:

```sh
tree-sitter highlight some/scala/file.scala
```

And see the syntax highlighting spit out. This is also the format used for
testing, so it provides an easy way to get the output we use for testing.

## Generation

In order to help not cause conflicts with multiple prs being open at the same
time, we don't check in the parser on each pr. Instead, just check in the
changes to the `grammar.js` and the CI will take care of the rest. Each night if
changes are detected it will automatically be generated.

## Smoke testing

You'll noticed that there is a part of CI that checks parsing against the Scala
2 and Scala 3 repositories to ensure we don't introduce unexpected regressions.
The script for this is in `script/smoke_test.sh`. If you're change is increasing
the coverage in either the library or compiler, please do update the expected
percentages at the top of the file.

## Obtaining an error reproduction

_With Neovim_

When creating an issue you'll want to ensure to include the `ERROR` node in it's
context. There's a couple ways to do this. If you're using Neovim and utilizing
treesitter, you can see the tree with `:lua vim.treesitter.show_tree()`. You can
copy it directly from the open panel.

_Manually_

A manual way to get this would be to ensure that when you're in the repo you
have the latest parser with

```sh
npm run build
```

Then you can copy your Scala code in a file and pass that file into `npm run
parse`:

```
tree-sitter parse <path/to/your/file.scala>
```

Then the tree will be printed out for you to copy.
