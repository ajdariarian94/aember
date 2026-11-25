# test-support

Header-only helpers for aember's C++ tests (GoogleTest), included through
`${AEMBER_TEST_SUPPORT_DIR}` (set in `libraries/CMakeLists.txt` and
`services/CMakeLists.txt`):

- `aember-test/property.h`: property tests. `aember::test::ForAll(runs,
  [](Gen& g) { ... EXPECT_...; })` checks a rule on `runs` generated cases.
  A failing case prints `AEMBER_PROPERTY_SEED=<seed> AEMBER_PROPERTY_RUNS=1`,
  which reruns exactly that case; a larger `AEMBER_PROPERTY_RUNS` runs more.
- `aember-test/dependency-graph.h`: random dependency graphs (name -> its
  dependencies, as in services.json / containers.json) with cycles,
  self-loops, unknown names and duplicates (`RandomGraph`, `RandomDag`), and
  the reference answers a scheduler is checked against: `Startable` (what
  `spec/tla/StartGraph.tla` starts: everything not in a cycle and not
  depending on an unknown name or such an entry), `DependenciesFirst` (an
  order starts each entry after its dependencies, once), `Depth` (the
  longest chain: the fewest waves a parallel start needs).
