# Contributing to Awide Polar

Thank you for your interest in contributing to Awide Polar! This document outlines the guidelines and requirements for contributing to this project.

Awide Polar is an open source database based on PostgreSQL and [PolarDB for PostgreSQL](https://github.com/polardb/PolarDB-for-PostgreSQL), the open source project by Alibaba Cloud. Our goal is to grow the PostgreSQL community. Contributors are welcome to submit code and ideas.

## Before Contributing

- Sign the [Individual Contributor License Agreement](legal/INDIVIDUAL-CLA.md)
  (or have your employer sign the [Corporate CLA](legal/CORPORATE-CLA.md))
  via a [CLA signing issue](https://github.com/awide-labs/polar/issues/new?template=cla_signing.yml)
  or email to `info@awide.io`. A maintainer adds the `cla-signed` label on your
  pull request after verification.

## Steps

Here is a checklist to prepare and submit your PR (pull request):

- Create your own GitHub repository copy by forking [`awide-labs/polar`](https://github.com/awide-labs/polar).
- Check out the [architecture overview](polar-doc/docs/theory/arch-overview.md) and the [development guide](polar-doc/docs/contributing/contributing-polardb-kernel.md) for how to build and run Awide Polar.
- Run `make stylecheck` to format your code, and push changes to your personal fork.
- Write a detailed commit message following the Conventional Commits format (see below), and open a PR against the upstream repository.
- Wait for all CI checks to pass.
- Wait for review and address all feedback.
- Wait for merging.

## Commit Message Format

All commits must follow the [Conventional Commits](https://www.conventionalcommits.org/) specification. This is enforced by CI checks on all pull requests.

### Format

```
<type>[optional scope]: <description>

[optional body]

Refs: <reference>[, <reference>...]
```

### Rules

- **Header (first line)**: Must be 72 characters or less
- **Body lines**: Each line must be 72 characters or less
- **Refs footer**: Mandatory - must contain a comma-separated list of issue references (e.g., `Refs: PROJ-1234` or `Refs: PROJ-1234, PROJ-5678`). Only one `Refs:` footer is allowed per commit

### Valid Types

| Type | Description |
|------|-------------|
| `feat` | A new feature |
| `fix` | A bug fix |
| `docs` | Documentation only changes |
| `style` | Changes that do not affect the meaning of the code (formatting, etc.) |
| `refactor` | A code change that neither fixes a bug nor adds a feature |
| `perf` | A code change that improves performance |
| `test` | Adding missing tests or correcting existing tests |
| `build` | Changes that affect the build system or external dependencies |
| `ci` | Changes to CI configuration files and scripts |
| `chore` | Other changes that don't modify src or test files |
| `revert` | Reverts a previous commit |

### Scope Requirement

For `feat`, `fix`, and `perf` commits, we strongly recommend including a scope
to maintain traceable commit history and enable automation. The scope identifies
which component or feature the commit relates to.

**Why scopes matter:**

- Makes commit history easier to navigate and search
- Enables automated changelog generation per component
- Helps reviewers quickly understand the affected area
- Ensures related changes can be easily tracked together

**Guidelines:**

1. When introducing a new feature, choose a short, descriptive scope name
2. For subsequent commits related to the same feature, use the same scope
   consistently
3. Check existing scopes before creating a new one to avoid duplicates or typos

**Example:** If a feature was introduced with `feat(wal pipelining): add WAL
pipelining support`, related fixes should use `fix(wal pipelining): ...`

To find existing scopes in the repository, run:

```bash
git log --format="%s" | grep -oE '^(feat|fix|perf)\([^)]+\)' | \
  sed 's/[a-z]*(\(.*\))/\1/' | sort -u
```

CI will warn (but not fail) if a `feat`/`fix`/`perf` commit is missing a scope
or introduces a scope not previously used in the repository.

### Optional Footers

In addition to the mandatory `Refs:` footer, you may include other optional
footers following the Conventional Commits specification. Common examples
include:
- `Skip-changelog: true` - Skip changelog requirement (see Changelog section)
- `See: <URL>` - Reference to external documentation, RFCs, or related
  resources
- `Discussion: <URL>` - Link to a discussion related to this commit, e.g., a mailing list thread

All footers must follow the `token: value` format and are exempt from the
72-character line length limit.

### Exemptions

- **Merge commits**: Automatically generated merge commits are exempt from
  Conventional Commits validation
- **Commits brought by merges**: All commits that are brought in by merge
  commits (e.g., when merging from upstream PolarDB for PostgreSQL) are also
  exempt from validation, as we have no control over their commit message format

### Examples

```
feat(api): add user authentication endpoint

Implement JWT-based authentication for the REST API.

Refs: PROJ-1234
```

```
fix: resolve memory leak in cache handler

The cache was not properly releasing resources on cleanup.

Refs: PROJ-5678, PROJ-5679
```

```
docs: update installation instructions

Refs: PROJ-9012
```

```
perf: optimize index scan for large tables

Use bitmap index scans instead of sequential scans for queries with
large result sets. This improves query performance by reducing I/O
operations and memory usage.

Refs: PROJ-3456
See: https://www.postgresql.org/docs/current/indexes-bitmap-scans.html
```

**Example with Skip-changelog footer:**

```
fix: resolve internal deadlock in test suite

This fixes a deadlock condition that only occurs in the test
environment and does not affect production. The issue was caused by
improper lock ordering during parallel test execution.

Skip-changelog: true
Refs: PROJ-7890
```

## Changelog Requirements

Commits that introduce user-visible changes must include an update to `CHANGELOG.md`. This is enforced by CI checks on all pull requests.

### Changes Requiring Changelog Updates

- `feat` - New features
- `fix` - Bug fixes
- `perf` - Performance improvements
- Breaking changes (indicated by `!` after type/scope or `BREAKING CHANGE:` in body)

### Changelog Format

We follow the [Keep a Changelog](https://keepachangelog.com/en/1.1.0/) format. Add your changes under the `[Unreleased]` section in the appropriate category:

- **Added** - for new features
- **Changed** - for changes in existing functionality
- **Deprecated** - for soon-to-be removed features
- **Removed** - for now removed features
- **Fixed** - for any bug fixes
- **Security** - in case of vulnerabilities
- **Performance**: performance improvements (our extension to Keep a Changelog)

### Entry Format

Each changelog entry (paragraph starting with `- ` at the first column) must end with a Jira issue reference in the format `(PROJ-NNNN)`, optionally followed by `.` or `:`. This is enforced by CI checks.

**Valid examples:**

```markdown
- Add new configuration parameter `polar_enable_parallel_ddl` (PROJ-1234)

- Reduce contention on the flush list on RW node by splitting it into multiple
  partitions (currently 64), with each partition having its own lock,
  control structure and statistics (PROJ-5678)

- The following third-party extensions have been removed (PROJ-9012):
  - hll
  - log_fdw
  - pase
```

### Example

```markdown
## [Unreleased]

### Added
- New configuration parameter `polar_enable_parallel_ddl` (PROJ-1234)

### Performance
- Optimized index creation for large tables (PROJ-2345)
- Reduced memory usage in query planner (PROJ-3456)
```

### Skipping Changelog Updates

If a commit with user-visible changes intentionally does not require a changelog update, add the following footer to the commit message:

```
Skip-changelog: true
```

## Coding Style

### Languages

- PostgreSQL kernel, extensions, and related tools use C to remain compatible with the community version and to upgrade easily.
- Management-related tools can use shell or Perl for efficient development.

### C Style

- C code follows PostgreSQL's programming style, including naming, error message format, control statements, line length, comment format, and the length of functions and global variables. For details, see [PostgreSQL style](https://www.postgresql.org/docs/current/source.html). Highlights:

  - Code in PostgreSQL should only rely on language features available in the C99 standard
  - Do not use `//` for comments
  - Both macros with arguments and static inline functions may be used. Prefer static inline functions when they simplify the code.
  - Follow BSD C programming conventions
  - C code must be formatted using `pgindent` before committing. The CI pipeline automatically checks code style compliance on all pull requests. Requirements:
    - `pg_bsd_indent` version 2.1.2 must be installed
    - Run `src/tools/pgindent/pgindent` to format your code
    - Ensure all C files pass the pgindent check before pushing

- Shell programs can follow [Google code conventions](https://google.github.io/styleguide/shellguide.html)
- Perl programs can follow the official [Perl style](https://perldoc.perl.org/perlstyle)

### Code Design and Review

We share the same thought and rules as [Google Open Source Code Review](https://github.com/google/eng-practices/blob/master/review/index.md).

Before requesting code review, run unit tests and pass all tests under `src/test`, such as regress and isolation. Submit unit tests or functional tests together with your code changes.

This section summarizes the full cycle of high-quality development, from design and implementation through testing, documentation, and code review. Consider the following during development and review:

- The code is well-designed.
- The functionality is good for the users of the code.
- Any UI changes are sensible and look good.
- Any parallel programming is done safely.
- The code isn't more complex than it needs to be.
- The developer isn't implementing things they might need in the future but don't know they need now.
- Code has appropriate unit tests.
- Tests are well-designed.
- The developer used clear names for everything.
- Comments are clear and useful, and mostly explain why instead of what.
- Code is appropriately documented.
- The code conforms to our style guides.
