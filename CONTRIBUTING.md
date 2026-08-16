# Contributing

hax is developed with coding agents, as most contributions to it will be. Whether a model wrote the
patch is not the interesting question: code generation is cheap, while identifying worthwhile
problems and delivering thoroughly validated changes is not. This guide is about the expensive part.

## Start with the problem

For features, behavioral changes, and non-trivial fixes, open an issue first. Keep it concise and
describe in your own words:

- what happened or what you want to accomplish;
- a concrete example or minimal reproduction;
- why it matters; and
- the desired behavior.

Check [`docs/philosophy.md`](docs/philosophy.md) before proposing new functionality: many omissions
are deliberate. Please wait for agreement on the problem and scope before implementing it. Small,
obvious corrections do not need prior discussion.

## Prepare the change

[`AGENTS.md`](AGENTS.md) is the working reference for build, test, style, and architecture; the
[README](README.md#from-source) covers installing the build dependencies.

Do not submit first-pass agent output. Before opening a pull request:

- run agents from the repository root so they read `AGENTS.md`;
- keep the change to the smallest coherent scope;
- cover new behavior with a test, or say why none applies;
- run `make tests` and `make lint`;
- run `BUILD_DIR=build-asan make tests` when the change touches allocation or object lifetimes, and
  `build-tsan` when it touches threads;
- manually verify user-visible behavior where applicable;
- have a fresh model independently review the issue and the completed change, preferably from a
  different model family; and
- personally inspect the result and resolve or account for the review findings.

The independent review should inspect the relevant complete files, not only the diff, and look for
correctness problems, regressions, unnecessary complexity, and violations of project conventions.

## Open the pull request

Link the motivating issue and concisely state:

- why the change is needed;
- what changed;
- exactly how it was verified and how a reviewer can confirm it; and
- which models implemented and independently reviewed it.

Open the pull request only when you consider it merge-ready. Generated prose is fine as an editing
aid, but issues, pull requests, and review responses must reflect claims and reasoning that you
personally understand and can substantiate.

Maintainers may close unclear, undiscussed, oversized, or insufficiently validated changes without
detailed review. Unattended or bulk-generated submissions are not accepted.

## Licensing

By submitting a change you affirm that you have the right to contribute it and that it is licensed
under the project's [MIT license](LICENSE). Whether or not an agent wrote it, the code is yours:
you are responsible for what it does.
