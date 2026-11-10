# Development Notes on Korvid Engine

This is as much an experiment in large scale AI-assisted development as it is a
game engine project. I have had variations of this engine in my head for almost
20 years now, but never the time to write it the way I wanted to. So one fine
day in 2026 I thought "what the heck, AI exists, maybe I can teach it to write
a decent engine". This is the result.

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

    What are my options for cross-platform graphics? I want to build a game engine that could ideally handle <=AA games. For v1, a 2.5D flexible sprite+bg engine would work, but I want a migration path to something that hits 25-50% of last year's cutting edge. My assumption is that thoughtful engine design will still leave most games CPU-bound, an issue I intend to address by moving much of the game logic to a higher-level language that makes concurrency easier.

Followed after some pedestrian back-and-forth (that argued with my assumptions) 
with this:

    OK. some ground rules for the engine. These should be critiqued first. It makes sense that there will be at least one full translation of the game's representation of the universe (probably a scene graph), and an internal driver model (pluggable back ends) means the pre-translation version is also probably the lingua fraca of the engine bus. So the outer engine works entirely on the abstraction and passes it around or exposes it to itself and the game code. The inner engine works off snapshots of that abstraction, mostly a thin layer around whatever back end is responsible for the video. Audio can be done exactly the same way. I/O needs its own abstraction that accommodates whatever parallelism is possible on each platform. Scene graph should probably be a DAG. The last architecture I used and liked (and wrote) used entity pools, where entities owned nodes in the graph. Update waves could proceed through the pools or the graph, and sometimes did both. The abstractions should be thin: Graph nodes, models and textures in memory should resemble the most common practical case. Let's start there

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

I had the AI take a paragraph on what I wanted each slice to look like, and then
spit it back to me with commentary. We'd proceed until I thought it had a 
decent grasp on what I wanted and a plausible plan for getting there, and I'd
have it write out a small dev-focused PRD for the slice. This lays out the 
rationale, the requirements, exit criteria, and milestones. Yeah, we run Agile, 
but I think this is more just about good project breakdown and scoping than any
specific development methodology.

One of the prompts that led to the creation of the engine slice 4 PRD:

    Time for slice 4 of Korvid. This one is focused on completing simple game support. I think it should include text (ideally rendering ttfs loaded as assets), basic sound (replay wav or other common sound formats with pitch and volume adjustments), pause mode, and the beginnings of the node graph so sprites can be children of other sprites. Anything else make sense in there?

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

Another thing that just made everything better was the modular structure of the
engine. I wanted that from the beginning, but it worked out even better than I 
thought it would. It helped the AI stay focused on the task at hand, it added
clear boundaries to differentiate public from private interfaces even within the 
engine, and it eventually led to target games and host tools being able to very 
naturally share modules.

The build system worked out well too. I have spent more weeks than I care to 
recall wrestling with multi-platform build systems, and there's _still_ nothing 
out there that makes it pleasant. Ninja at least is flexible enough to do some
of it, and a custom wrapper on that with a little bit of knowledge about how I
wanted it to all work turned out to be quite nice, easy to write, and easy 
enough to understand that you can actually touch the build system without it
breaking.


