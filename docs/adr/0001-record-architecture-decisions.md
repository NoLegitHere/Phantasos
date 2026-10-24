# ADR 0001: Record architecture decisions

- Status: accepted

## Context
Phantasos makes many long-lived design choices: the memory model, the ABI,
bootstrap strategy, and the toolchain scope. Later contributors need to know
why each choice was made.

## Decision
Each significant decision is recorded as a numbered ADR in `docs/adr/`.

## Consequences
ADRs are never rewritten. When a decision changes, a new ADR is added that
supersedes the old one.
