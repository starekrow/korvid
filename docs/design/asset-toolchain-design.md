# goldberg — Asset Orchestrator Design

Nov 13, 2026 · @starekrow

## Summary

goldberg is Korvid's asset orchestrator: a declarative, self-assembling toolchain that turns source assets into runtime data and pushes them live into a running target. Users declare what an asset *is* and where it comes from; goldberg derives how to process it, in what order, and where it ends up.

The thesis it serves: Korvid has no canonical editor. It has a running world that anything can author into. goldberg is the broker between authoring tools (Aseprite, Blender, Tiled/LDtk, Korvid's own editors) and that world, both live during iteration and as a batch build for shipping. Iteration and pack builds are one dependency graph with different sinks: a socket to a running target, or a pack file.

goldberg is also the first serious test of Korvid as a host for tools: it is built on the engine's own I/O, data modeling, and UI.

## Goals and non-goals

**Goals**

- A full-fledged engine tool with both a UI and a CLI, sharing one core.
- Uses the engine's data modeling, pools, I/O, and rendering for its own purposes.
- Removes procedural pipeline knowledge from humans: no remembering which tool skin files go through, or which textures must build early for a composite. Order falls out of graph topology.
- One dependency truth for live link and pack builds, so hot reload and shipped data can't drift apart structurally.
- Explicit edges always work. Inference is shorthand that can be expanded into explicit declarations at any point.
- Every decision is traceable: what path an asset took, and which rule chose it.
- Drives creation of a dialog UI library for the engine.

**Non-goals**

- goldberg does not run game code. It is not a play-in-editor host.
- goldberg does not design around how any particular game uses the engine. It consumes engine capabilities as a client like any other.
- goldberg is not the canonical editor. The example editors (skeleton builder, mapper, animator) are separate tools that author into the graph.

## Architecture

goldberg is a Lua application running on a thin layer of C++ primitives. C++ appears only where it makes something more understandable on one side or the other: memory layout, I/O, and engine services.

**C++ layer**

- A pool type exposed to Lua. Kinds are declared in C++, which owns the kind layout (field types, offsets, packing); Lua owns everything else that the pool holds and does.
- Engine I/O: async, event-driven, one abstraction across files, sockets, and processes.
- Engine UI and rendering, for goldberg's own windows and previews.
- Hashing, process spawning, filesystem watch.

**Lua layer**

- The asset model, converter registry, graph resolution, invalidation, lockfile, packing policy, and reporting.
- The CLI and the UI are two front ends over the same Lua core.

**Process topology**

- goldberg is always out of process from what it works on. It can run on a different host from the target with the same code.
- Each authoring tool gets its own monitor process (Aseprite, Blender, Korvid's animation editor), each plumbed over its own socket into goldberg. A monitor may have its own host UI.
- Targets (PC builds, devkits) connect over sockets and receive pushed updates by handle.
- Game-specific asset processing is supplied by the game and runs as separate converter processes over the same socket protocol as the tool monitors, so goldberg never runs game code internally.

&#91;embedded content: goldberg process topology · 3 tools, 2 sinks\]

One graph feeds both sinks: a socket to a running target during iteration, and pack files for shipping.

## Asset model

An asset is a declaration: a handle, a source, and a runtime type. Everything else (processing path, build order, pack placement) is derived.

- **Handles.** Runtime code refers to assets by handle. A handle resolves to the best version currently available, which is what makes live import and background refinement possible.
- **Types and subtypes.** Types are a lattice, not a flat list. A texture is sRGB or linear, color or normal map or mask, straight or premultiplied alpha. Subtypes exist so that inference can't find a path that type-checks but is wrong.
- **Sources vs derived nodes.** Sources are authored inputs and are immutable from the graph's side. Derived nodes are outputs of converters. Derived metadata lives on derived nodes, never written back onto sources by the graph.
- **Metadata conventions.** Declarations can live in sidecars, extended attributes, in-asset metadata, or project data. The conventions themselves are pluggable and can be extended.
- **Defaults and overrides.** Directory-scoped defaults cover the common case (a PNG in `sprites/` is a sprite). Per-asset declarations override them. Parameters such as compression quality, pivots, and atlas padding flow the same way.
- **Capabilities.** An asset declares which authors may write into it, for example `accepts $tool2 updates`. This is the attachment point for live-link sources and reverse mutators.

## The graph

The toolchain is a DAG that assembles itself from declarations and a library of converters. Nobody writes the pipeline; it falls out of what converters are registered.

- **Converters are mutators with declared signatures.** Each declares an input type and an output type, for example `.aseprite → frame set`, `frame set → atlas`, `atlas → BC texture (target X)`. Any converter that fits the signatures composes with any other.
- **Path inference.** For each asset, goldberg searches the converter graph for a path from source type to declared runtime type.
- **Ambiguity rule.** Infer when the path is unique; require a declaration when it isn't. Never silently pick one of several paths.
- **Fan-in.** Some nodes depend on a set (an atlas over many sprites). Set membership is declarative too, so adding a file to a directory invalidates the atlas.
- **Dynamic dependencies.** Some edges are only discoverable by processing (a .blend references textures; a level references prefabs). Converters report discovered edges as they run, and the graph grows during the build. This is the sense in which the toolchain is self-assembling over time.
- **Explicit edges.** Any inferred edge can be written explicitly. A project can drop to explicit edges wherever inference gets confusing; the worst case is a hand-written pipeline.

## Invalidation and caching

Invalidation decisions rest on content hashes; timestamps are only a cheap pre-check for whether a file needs hashing.

- **Why hashes decide.** Once write-back exists, an identical write must be a no-op. With timestamp invalidation, a reverse mutator that rewrites unchanged content bumps mtime, fires the watcher, and loops.
- **Content-addressed cache.** A derived node's key is its input hashes, its parameters, and its converter version.
- **Converter purity.** Converters must be pure functions of that key. External tools often aren't (embedded timestamps, version drift, float noise in headless exports). Their version is part of the key, so upgrading a tool invalidates what it produced.
- **Triggers.** Update conditions are modeled as triggers, so they walk the graph like anything else.
- **Build everything.** Always available. It's a real toolchain.

## Lockfile

Resolution runs once and is pinned in a lockfile; the lockfile is both the stability mechanism and the provenance record.

- **Pinned paths.** Every asset's resolved converter path, parameters, and governing rules are recorded.
- **Re-routes are diffs.** Registering a converter is a global change: a cheaper path can re-route assets nobody touched. With a lockfile, that shows up as a diff someone accepts, not as assets that silently look different.
- **Provenance.** "Why did this asset end up like this?" is answered by reading the lockfile, not by re-running inference. Pack membership, tag precedence, and hoisting decisions are recorded the same way.

## Live link and build profiles

Iteration and shipping share the graph structure but not the parameters; profiles make that difference explicit.

- **Profiles.** A fast iteration profile (cheap encodes, minimal processing) and a ship profile (full-quality encodes). The graph is the same; converter parameters differ by profile. "Fine in hot reload, broken in the pack" becomes a profile bug, not an architecture bug.
- **Background refinement.** goldberg uses idle time to build the ship-quality version of recently changed assets. When it finishes, the handle rebinds on target, either on a keypress or automatically.
- **Best-available handles.** The target holds the draft until the refined build lands. This generalizes mip streaming to every asset type.
- **Draft vs final visibility.** A refined asset can differ from its draft in memory footprint, load time, and sometimes behavior (collision, nav data). Each asset's draft/final state is visible on target, so nobody tunes a frame budget against draft data.

## Authorship and write-back

Anything that writes to a source is an author, and authors sit outside the graph; that keeps the graph acyclic even with write-back.

- **Reverse mutators.** A game module or tool may write back to a source: in-engine bakes (lightmaps, navmesh), collision or timing tuned in play, a play-mode tweak captured as an authored edit. A reverse mutator writes a new source revision through the same front door a human save does. It is never a graph node.
- **Capability declarations.** An asset only accepts writes from authors it declares (see Asset model).
- **Write-scoped suppression.** A mutator that doesn't want a refresh on its own write tags that write with an origin or revision ID; the trigger ignores only events carrying that tag. Suppression is never a time window: a window swallows concurrent edits (an artist saving during a bake) and can stick on if the mutator crashes.
- **Hashes as backstop.** An untagged event with identical content is a no-op by content hash.

## Packing

Packs are tag closures over the graph, and every packing policy is a query over that graph.

- **Tag closures.** Tag a world file `level-1` and everything it references comes along. Tag music, movies, and anything else not reachable by reference. Then tell the packager to pack `level-*` into files.
- **Shared assets.** An asset in more than one closure is auto-hoisted into a common pack, with a warning past a fragmentation threshold.
- **Force-embed.** A pin that duplicates an asset into each pack that needs it, for load locality on slow media.
- **Tag precedence.** Whether an explicit tag beats an inherited one has one defined answer, and the lockfile records which rule decided.
- **Budgets.** A pack's closure size is known at resolve time, so over-budget packs and the assets responsible are reported at build time, not at the load screen.
- **Single-namespace fallback.** Assets resolve by name in one namespace; a miss in the expected pack falls through to another source. A packing error becomes a longer load instead of a crash.
- **Fallback limit.** Fallback assumes the bytes are reachable. With chunked installs, streaming downloads, or DLC, the resolver must distinguish "slow" from "not here."
- **Runtime load telemetry.** The running game reports loads that came from outside the expected pack. In dev and CI these are loud (red, blinking, able to fail the build); in ship builds they are a quiet hiccup plus a telemetry line.
- **Undeclared runtime references.** Loads by computed string (`"enemy_" + variant`) are invisible to static closure. Either runtime references go through declared handles, or telemetry from playtests feeds observed loads back as edges.

## First-adopter experience

A newcomer should get a faster iteration loop than anything they've used without writing a single rule; whether that's a good trade depends on the explain tool and the error messages, not on the graph.

- **Zero-config day one.** Drop a PNG into `sprites/`, it appears in the running game; save in Aseprite, it updates on screen. If day one requires a declaration, adoption fails on day one.
- **The bridge.** For Unity users: it's `.meta` files, except set per directory and composable.
- **Explain.** An inspector equivalent, at minimum `goldberg explain <asset>`: type, path, and the rule that decided each. Without it, the first surprise reads as magic going wrong.
- **Errors are the onboarding.** A misclassification error names the asset, the rule it matched, and the one-line declaration that overrides it. Never a stack trace from a converter three steps down.
- **Visible cliff.** Assets matched by fallback rules carry a soft warning ("consider declaring this"), so the declaration language isn't first learned during crunch.
- **Disclosure order.** Folder conventions and live link immediately; tags and packs at first ship; lockfile when something re-routes; custom converters rarely; reverse mutators, capabilities, and hoisting policy at medium-project scale or never.

## Dialog UI library

goldberg's UI is built entirely on engine rendering and drives a new dialog UI library for Korvid; the UI and CLI are two front ends over the same core.

What goldberg needs the library to support:

- **Inspection.** The UI form of `explain`: an asset's type, resolved path, parameters, and governing rules, with links to upstream and downstream nodes.
- **Diff review.** Lockfile re-routes presented as diffs to accept or reject.
- **Live status.** Per-asset draft/final state, background refinement progress, and connected monitors and targets.
- **Reports.** Warnings (fallback matches, out-of-pack loads, fragmentation), pack budgets, and the assets responsible.
- **Forms.** Editing declarations and per-asset overrides without hand-editing sidecars.

Since goldberg uses the engine's UI for its own purposes, this library is general engine UI, not goldberg-specific. Any game or tool built on Korvid inherits it.

## Open questions

- [ ] Game-supplied converters: decided, separate converter processes over the monitor protocol.
- [ ] Pool kinds: decided, declared in C++ and exposed to Lua.
- [ ] Declaration format: Lua tables, a data format, or both?
- [ ] Tag precedence: does an explicit tag override inherited tags, or add to them?
- [ ] Purity policy for external tools: which nondeterminism gets normalized, and which just invalidates?
- [ ] Protocol versioning between goldberg builds and target builds.
- [ ] Lockfile scope: one per project, or one per pack or profile?
- [ ] Fragmentation threshold for the auto-hoist warning: fixed, or per platform?
