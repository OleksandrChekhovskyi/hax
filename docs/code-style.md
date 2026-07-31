# Code readability and comments

Code should communicate its behavior primarily through names, types, structure, and tests. Comments
are for important information the code cannot express clearly.

## Make the code explain itself

- Use the shortest name that is unambiguous in its scope. Include purpose, ownership, units, or
  state when relevant, but do not repeat context supplied by the containing function, type, or
  module. Prefer `window_rows`, `bytes_read`, and `timeout_ms` to `wh`, `r`, and `timeout`; avoid
  names such as `bytes_read_from_child_process_pipe` when `bytes_read` is clear. Conventional names
  such as `i`, `fd`, `buf`, and `ctx` are fine when their meaning is established by the type or API
  and remains obvious throughout the scope; expand them when multiple instances or roles coexist.
- Keep functions at one level of abstraction and control flow straightforward. Extract a named
  operation when that makes a commented block unnecessary; do not create trivial wrappers merely
  to avoid a comment.
- Represent meaningful states with enums or separate types instead of undocumented integers or
  clusters of interacting booleans.
- Put validation and invariants close to the data or operation they protect. Make invalid states
  difficult to construct where practical.
- Remove obsolete parameters, branches, and abstractions. Do not explain why dead structure remains.

## Write comments for durable information

A useful comment records something a maintainer needs for correctness but cannot readily infer:

- why an obvious or simpler approach is wrong;
- ownership, lifetime, nullability, sentinel, error, or threading contracts;
- non-local invariants and ordering requirements;
- security, portability, protocol, or platform constraints;
- the intent behind an inherently subtle algorithm.

State the constraint directly and as briefly as possible. Prefer present-tense explanations of why
the code must have its current shape. Put `/*` beside the first line of text and `*/` beside the
last rather than on delimiter-only lines.

```c
const char *provider_name; /* borrowed; valid for the provider's lifetime */

/* Build before fork; the child may call only async-signal-safe functions before exec. */
char **envp = build_child_env();

/* write(2) may clear set-ID bits, so restore the mode after writing. */
(void)fchmod(fd, mode);
```

## Do not use comments to

- narrate the next statement or restate names and control flow;
- describe previous implementations, fixes, patches, or reviewer discussions;
- list every current caller, backend, test, example, or implementation step;
- duplicate user documentation or architecture documentation;
- speculate about future features;
- compensate for unclear naming or unnecessarily complicated structure.

History belongs in version control. User-facing behavior belongs in `docs/`. Broad architecture
belongs in `AGENTS.md` or a focused design document. Tests should express regressions through a
specific test name, fixture, and assertions; add a comment only when the scenario remains unclear.

## Keep contracts local

- Header comments describe the public contract: inputs, outputs, ownership, lifetime, errors, and
  constraints callers must honor.
- Source comments explain local rationale and implementation invariants, not the interface again.
- Field comments clarify semantics that the field's name and type cannot carry.

When changing code, verify nearby comments still add information and remain true. Shorten, move, or
delete them when they do not. A long comment is a prompt to consider better structure or a focused
document, not an automatic requirement to split the code.
