# tisa

tisa specifies tensor accelerator instruction semantics. The three example
programs live in [examples](examples) and use the `.tisa` extension.

Build and test with Bazelisk or Bazel 9.3.3.
The first build generates the parser and compiles Z3 for static validation.

```sh
bazel test //...
```

Run the overlapping slice copy example from the repository root.

```sh
bazel run //:tisa -- examples/slice_copy.tisa slice_copy 1 0 4 -- buf=10,20,30,40,50,60
```

It prints `buf=10,10,20,30,40,60`. Operands follow the declaration order.
Supply each state bank once as `BANK=v1,v2,...`. Flatten tile banks by tile,
then by row within each tile.

The C++ API exposes `Parse` in [parser.h](src/parser.h) and `Validate` and
`Execute` in [semantics.h](src/semantics.h). Validation checks expression shapes,
access bounds, and declared regions under the instruction's preconditions.
Execution checks that results fit their destination element types before
committing all writes together.
# tisa
# tisa
