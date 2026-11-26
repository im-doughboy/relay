# Working on Relay

Read this before starting any work in this repository.

## Plan

- **ROADMAP.md is the plan.** Check it before starting anything.
- **Bug fixes** (something broken now) can go ahead.
- **Features** go in the release the roadmap puts them in, in order: 0.6 phone setup is next.
- **Anything not on the roadmap** is proposed to the owner first (what it is and why it matters),
  and added to ROADMAP.md once agreed. Don't start it before that.
- When a roadmap item is done, tick its "Done when" boxes in ROADMAP.md in the same commit.

## Working together

- More than one Claude session may work on this repo. Pull before editing, and keep to the files
  your task needs. If another session said it is working in some files, leave them alone until it
  pushes.
- The webOS port lives in `platform/webos/` and shares the code in `src/` and `qml/`. A change to
  shared code must build and pass the tests on Windows and webOS.

## Rules

- Commit as `git -c user.name=im-doughboy -c user.email=im-doughboy@users.noreply.github.com commit`.
  No `Co-Authored-By`, `Claude-Session` or other AI attribution lines in commit messages or pull
  requests. Never put the owner's personal email address, provider logins or stream addresses in code,
  commits, logs, tests or docs.
- API tokens (TMDB, Trakt, GitHub) live only in the user's settings or the system's credential
  store, never in the code.
- Run the tests before pushing, and check both CI builds (Windows and webOS) after pushing.
- User-visible changes go in CHANGELOG.md, written for the people using Relay.
- Don't push tags or publish releases: the owner releases by pushing a version tag.
