# Development Notes on Korvid Engine

This is as much an experiment in large scale AI-assisted development as it is a
game engine project. I have had variations of this engine in my head for almost
20 years now, but never the time to write it the way I wanted to. So one fine
day in 2026 I thought "what the heck, AI exists, maybe I can teach it to write
a decent engine". This is the result.

My overall take: This was an amazing success, if AI working well means success
to you. I went from an empty git repo to a game engine that runs on Linux,
Windows, MacOS, and iOS in *five days*. With fancy thread-aware allocators,
pipelined delta GPU updates, a fully modern backend renderer, determinism, a
runtime-inspectable data model, a source code structure designed for modularity
in the game and the engine, slangc shaders on all platforms, minimal 
dependencies, some decent built-in helpers, and a suite of examples with one 
full-fledged game.

In my day job I manage teams of developers, and we're wrestling with the same
issues with AI that every other dev group is: How much to use, how to feel about
it, how to make sure that what we're building is still safe for users, and
what software engineering even is when the computer can do better work than some 
junior engineers.

Some of the lessons we landed on:

 - AI benefits from the same development practices that make a human team more
   effective: Good process, good documentation, measure what matters.
 - No really, if you manage devs and haven't read _Accelerate_, go buy it now.
 - Good engineering is not just code that works. Human judgement is 100%
   indispensable.
 - "Prompt engineering" is almost useless without real engineering experience.
    - For prod, that is...
    - For the thousand-and-one crappy little tools that don't have a security 
      surface or access to PII, you can do a heck of a lot with a couple of 
      prompts.
 - Security is harder than ever

With all that said, I wanted to find out where the edges were. I know a little 
bit about cross-platform game engine design, I used to do it professionally. And 
some of the problems involved are actually pretty hard to solve. At the same 
time, there's been a _ton_ written about game coding, and some very high quality
engines are source-available, so there's a lot of reference material out there. 
I figured that it would either start doing dumb things right off the bat and 
need heavy code review, or do OK. It ended up doing pretty well, I think.

I'm not sure yet whether I'll include all the prompts in the docs repo, but
the ones that kicked off development were these:

> What are my options for cross-platform graphics? I want to build a game engine
> that could ideally handle <=AA games. For v1, a 2.5D flexible sprite+bg engine
> would work, but I want a migration path to something that hits 25-50% of last
> year's cutting edge. My assumption is that thoughtful engine design will still
> leave most games CPU-bound, an issue I intend to address by moving much of the
> game logic to a higher-level language that makes concurrency easier.

Followed after some pedestrian back-and-forth (that argued with my assumptions) 
with this:

> OK. some ground rules for the engine. These should be critiqued first. It
> makes sense that there will be at least one full translation of the game's
> representation of the universe (probably a scene graph), and an internal
> driver model (pluggable back ends) means the pre-translation version is also
> probably the lingua fraca of the engine bus. So the outer engine works
> entirely on the abstraction and passes it around or exposes it to itself and
> the game code. The inner engine works off snapshots of that abstraction,
> mostly a thin layer around whatever back end is responsible for the video.
> Audio can be done exactly the same way. I/O needs its own abstraction that
> accommodates whatever parallelism is possible on each platform. Scene graph
> should probably be a DAG. The last architecture I used and liked (and wrote)
> used entity pools, where entities owned nodes in the graph. Update waves could
> proceed through the pools or the graph, and sometimes did both. The
> abstractions should be thin: Graph nodes, models and textures in memory should
> resemble the most common practical case. Let's start there

It didn't like my DAG scene graph at all, but took the rest of it. Some more 
details on the actual development process:

 - I asked for feedback constantly, or told it to stop for review.
 - I made it write a full PRD for the engine first, after a bunch of back and 
   forth in the chatbot laying out what I wanted to see and how I wanted it to 
   work. 
     - This only took part of an evening, but I was already quite opinionated 
       on engine design.
     - The idea behind the PRD was to build a scaffold that the AI could use
       to stay focused on the big picture without having to "remember" it - 
       keep it in context.
 - With the full PRD in hand, I started carving off "slices". I do this with my
   teams too. It helps keep them focused on a near-term deliverable, even when
   the project is huge.
 - Each slice has a coherent reason for existing. Something like "prove that 
   you can write a little game with these capabilities" or "make the renderer
   do something visible"
 - Each slice is further broken down into milestones, that help structure the 
   work without being too prescriptive. The code is done when the milestone is
   reached or when you've proven that it's a bad idea and you need a different 
   milestone.
 - The AI does very badly when asked to think. Ask it "what should I do next?"
   and it will lead off into some cul-de-sac no-one wanted. Ask it "does the
   current scheduler implementation really deliver on the design?" with a bunch
   of instructions on the kinds of things to look for, and it can do very well.
 - You need to keep a firm hand on the project. Every action the AI takes should
   improve the code in an intentional way. It's your responsibility as the dev
   to watch it and correct it. If you can't do that, you can't tell the 
   difference between what you were expecting and slop.

I had the AI take a paragraph on what I wanted each slice to look like, and then
spit it back to me with commentary. We'd proceed until I thought it had a decent
grasp on what I wanted and a plausible plan for getting there, and I'd have it
write out a small dev-focused PRD for the slice. This lays out the rationale,
the requirements, exit criteria, and milestones. Yeah, we run Agile, but I think
this is more just about good project breakdown and scoping than any specific
development methodology.

One of the prompts that led to the creation of the engine slice 4 PRD:

> Time for slice 4 of Korvid. This one is focused on completing simple game
> support. I think it should include text (ideally rendering ttfs loaded as
> assets), basic sound (replay wav or other common sound formats with pitch and
> volume adjustments), pause mode, and the beginnings of the node graph so
> sprites can be children of other sprites. Anything else make sense in there?

This might make it look like I was treating the AI as a person. You, the user of
the "AI" tool, should be very clear that it is not a person. It is, however,
_shaped_ like a person in its training and how it understands text. The inputs
that you give, including the human-isms, will help shape its output. If you
treat it like crap or like a dumb workhorse, it will respond like its training
said people on the internet respond to being treated like crap or like dumb
workhorses. The way AIs are trained bakes the shape of our own reactions into
them. So, shape your input for the kind of output you want.

I put all the docs the AI generated into the repo, both for reference and 
because I thought that it would be interesting to see the progression of the
development later on.

I gained a _amazing_ advantage early on by requiring the engine to be
deterministic. I was originally a bit skeptical about that as a top-line
requirement but it made testing all the weird little crap that shows up when you
start writing real game and engine code _so much easier_ to debug. And the AI is
much more patient about testing every batch of changes against single-threaded
and multi-threaded builds, and tracking down the source of any non-determinism.
Quite often those turned out to be bugs, not just instability. I had only
written one other engine that was deterministic enough to show any random replay
from starting conditions plus an input script, and even that one only worked
right because I kept the frame rate pinned at 60.

Here's an example of the AI tracking down a seemingly minor discrepancy in a 
control system that would have surfaced as an impossible-to-find glitch in any
other engine I've written _or_ worked with. Note that it just did this on its
own: With deterministic behavior as a requirement from the beginning, it made 
sure the replays matched 100%. Each of these outputs was interspersed with the 
AI running the game to watch how it behaved:

> The replay reproduces the same results (score 438,816, two bombs), but state
> hashes diverge at some ticks and it stopped one tick short of the file’s end
> ecord. I’m locating the ticks where they differ.

> The replay differences come and go across three stretches and always end the
> same, which points to tiny float differences in the stick’s direction. I
> suspect the sample clamps the override’s axis twice, so I’m checking whether
> clamp_unit gives identical bits when applied again.

> My quick test used a slightly different formula; testing the real one (divide
> by the length):

> Not the clamp either. Running both, recording and replay, with the hash after
> every system, to find the first one whose output differs:

> The control system’s output differs at tick 1635, so the ship’s inputs on that
> tick differ. Decoding the file around that tick to compare against what the
> replay applied:

> The file’s values look right; the move at 1635 is a unit-length direction.
> Testing whether clamp_unit changes that particular vector:

> The recorded value is 1.00000012, so the sample’s clamp alters it again on
> replay. I’m adding an exact override to platform_hid so the pilots and the
> demo’s hash stay unchanged.

> The replay now matches its recording tick for tick: 11,318 hashes, the file
> alone flying the ship. Checking that a damaged recording is caught, by
> changing one recorded stick value:

Note that "hash after every system" bit. Because the engine's data structures
are stable between each system that runs in a tick, you can go pull a hash 
after each one (more on the machinery that enables this later). In a dev build, 
no less - you don't need debug instrumentation for it.

I'm going to emphasize again just how awesome it is to be able to record hashes
of engine state whenever you want to, and expect them to remain identical across
runs. If you have an engine or want to write one, I highly recommend this
practice. I did a scheduler redesign and rewrite to fix the spaghetti it was
turning into, and it was able to compare the replays before and after the
scheduling change. The expectation was no change at all; instead it found a
discrepancy at tick 10292 where two objects spawned at the same time and were
run in a different order with the new scheduler. Turns out both were correct,
but it identified that there _was_ a change, and it could prove that the change
was harmless. As an engine author that is mind-blowing to me - I find tracking
down those things to be incredibly tedious work, even though it pays off in
spades. But the AI is just amazing at bisecting and diagnosing them.

Another thing that just made everything better was the modular structure of the
engine. I wanted that from the beginning, but it worked out even better than I 
thought it would. It helped the AI stay focused on the task at hand, it added
clear boundaries to differentiate public from private interfaces even within the 
engine, and it eventually led to target games and host tools being able to very 
naturally share modules.

Another decision that had unexpected benefits: I wanted a useful CLI interface
to the engine, which meant that anything I might want to look at through it
would need to be reflected, which meant that most of the notable engine
structures got a description. I didn't want to try this through C++ magic, 
because it always turns into punctuation soup and doesn't work with other
languages that you might want to use for game dev. So, it became just field
descriptions over structs. 

With field descriptions over structs, it becomes pretty easy to reflect just
about anything, and it doesn't matter how it's constructed or updated. I 
decided that the entity pools should all be reflected, and the AI suggested that
they could all be hashed to give a complete description of engine state, useful
for checking determinism. This also meant that the AI (and I) could go poke 
around inside the engine on any frame without having to operate a debugger.
It also makes logging easier - you can just call a function to dump any
reflected structure to a string. And it almost immediately led to tick phase
dependencies being expressed with the same names. I was worried that this would
be a (small) drag on performance throughout, but it all runs amazingly smoothly,
with almomst all of the work happening once, at startup.

The build system worked out well too. I have spent more weeks than I care to 
recall wrestling with multi-platform build systems, and there's _still_ nothing 
out there that makes it pleasant. Ninja at least is flexible enough to do some
of it, and a custom wrapper on that with a little bit of knowledge about how I
wanted it to all work turned out to be quite nice, easy to write, and easy 
enough to understand that you can actually touch the build system without it
breaking.

I won't get into content generation at all here - I think that games (and music
and art) are a uniquely human endeavor, and when I play a game I want to see the
human ideas behind it, not a machine-generated fun optimizer, or a
stochastically probable next game. There is a complicated thing going on here:
For someone working in their own domain, these tools can unleash new levels of
creativity and productivity when used well. In my domain, coding, I am
comfortable saying that I could have built anything in here, and my version
would have closely resembled what the AI did here - just 2 years later. Where it
deviated, I yanked it back onto my rails.

But for the real games I want to build on top of this engine, I'll be doing most
of the coding myself, using the AI as a smart auto-complete. It'll take longer,
but I'll feel better about selling the result with my name in the credits. And
I'll feel like I can own whatever kudos or criticism might follow. And, while I
have shipped games with my own artwork and sound in them, I would not consider
myself an artist or composer, so I will leave the decisions of how much AI
assistance is proper to them. If I'm doing my own art because I can't afford an
artist, y'all just get to suffer through my programmer art and compositions.
This is purely my personal opinion, and I've got no judgement in me for the
folks that use AI for everything they're not good at. But I probably won't enjoy
those games.

While Arcfire is an interesting demo, and having the LLM build a game-like
thing made it much easier to quickly prove out all of the engine support for a
real game, I would not suggest that you really try to play it. It's a crappy
game at best. What I hope is that someone will take this, rip out all the assets
and maps and behaviors, and build an awesome bullet hell on the game core for me
to play.

These notes will need some organization at some point.

Adding stable transcendental functions to the engine (to enable cross-platform
determinism) turned out to be very straightforward. I started out asking for a
libm replacement that was correct. The idea was that then you could swap in any
platform primitives that were correct and faster. That got shot down immediately
- no-one makes them correct, and the correct versions of some like `pow` were
powerfully slow. But, with a bit of tweaking I ended up with versions that are
more accurate than the platform libraries and about the same speed. And suddenly
my replays all matched across platforms - all 5, the desktops and mobiles.

You do lose some performance - the flags that prevent platform FP shenanigans
also prevent some nice optimizations. But my experience has been that the real
impact of that is pretty small. You're much more likely to lose your frame rate
to cache or overdraw or something than exactly how long sin() takes. If sin() is
the problem, you need to do something different anyway.

I'm really happy with the design of `goldberg`. I always wanted to make a self-
assembling toolchain for a game; asset cooking is such a pain in the ass. After
building the self-ordering in-tick systems, it was a natural next step to build
a DAG around the asset pipelines. Building it as an engine-based tool brings
together a bunch of capabilities that are usually quite hard in tools - good
multichannel async I/O, math that matches the game's, systems that can handle
tens of thousands of items quickly, safe parallelism. We get a lot of extras for
free, like state hashing, memory tracking, and a CLI to help you dig around
inside your cooker. So frikkin cool.

