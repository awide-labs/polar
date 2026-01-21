# Contributing to PolarDB

Thank you for your interest in contributing to PolarDB! This document outlines the guidelines and requirements for contributing to this project.

PolarDB for PostgreSQL is an open source project based on PostgreSQL and other open source projects. Our main target is to create a larger community of PostgreSQL. Contributors are welcomed to submit their code and ideas. In a long run, we hope this project can be managed by developers from both inside and outside Alibaba Cloud.

## Before Contributing

- Follow the instructions and sign [CLA](https://gist.github.com/alibaba-oss/151a13b0a72e44ba471119c7eb737d74) of PolarDB for PostgreSQL

## Steps

Here is a checklist to prepare and submit your PR (pull request):

- Create your own Github repository copy by forking `ApsaraDB/PolarDB-for-PostgreSQL`.
- Checkout documentations [Advanced Deployment](https://apsaradb.github.io/PolarDB-for-PostgreSQL/deploying/deploy.html) for how to hack PolarDB-PG.
- Run `make stylecheck` to format your code, and push changes to your personal fork.
- Edit detailed commit message following the Conventional Commits format (see below), and create a PR to upstream.
- Wait for all CI checks to pass.
- Wait for review and address all feedbacks.
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

### Optional Footers

In addition to the mandatory `Refs:` footer, you may include other optional
footers following the Conventional Commits specification. Common examples
include:
- `Skip-changelog: true` - Skip changelog requirement (see Changelog section)
- `See: <URL>` - Reference to external documentation, RFCs, or related
  resources
- `Discussion: <URL>` - Link to discussion related to this particular commit, e.g. mailing list discussion

All footers must follow the `token: value` format and are exempt from the
72-character line length limit.

### Exemptions

- **Merge commits**: Automatically generated merge commits are exempt from
  Conventional Commits validation
- **Commits brought by merges**: All commits that are brought in by merge
  commits (e.g., when merging from upstream) are also exempt from validation,
  as we have no control over their commit message format

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
  partitions (currently 64), with each partition having its own own lock,
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

- PostgreSQL kernel, extension and related tools use C, in order to remain compatibility with community version and to upgrade easily.
- Management related tools can use shell or Perl for efficient development.

### Coding Style

- Coding in C follows PostgreSQL's programing style, such as naming, error message format, control statements, length of lines, comment format, length of functions and global variables. In detail, please refer to [PostgreSQL style](https://www.postgresql.org/docs/current/source.html). Here is some highlines:

  - Code in PostgreSQL should only rely on language features available in the C99 standard
  - Do not use `//` for comments
  - Both, macros with arguments and static inline functions, may be used. The latter is preferred only if the former simplifies coding.
  - Follow BSD C programming conventions

- Programs in shell can follow [Google code conventions](https://google.github.io/styleguide/shellguide.html)
- Program in Perl can follow official [Perl style](https://perldoc.perl.org/perlstyle)

### Code Design and Review

We share the same thought and rules as [Google Open Source Code Review](https://github.com/google/eng-practices/blob/master/review/index.md).

Before submitting code review, please run unit test and pass all tests under `src/test`, such as regress and isolation. Unit tests or function tests should be submitted with code modification.

In addition to code review, this document offers instructions for the whole cycle of high-quality development, from design, implementation, testing, documentation to preparing for code review. Many good questions are asked for critical steps during development, such as about design, function, complexity, testing, naming, documentation, and code review. The documentation summarizes rules for code review as follows. During a code review, you should make sure that:

- The code is well-designed.
- The functionality is good for the users of the code.
- Any UI changes are sensible and look good.
- Any parallel programming is done safely.
- The code isn't more complex than it needs to be.
- The developer isn't implementing things they might need in the future but don't know they need now.
- Code has appropriate unit tests.
- Tests are well-designed.
- The developer used clear names for everything.
- Comments are clear and useful, and mostly explain why instead of what.
- Code is appropriately documented.
- The code conforms to our style guides.
