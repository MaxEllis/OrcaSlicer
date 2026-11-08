# AI in Slicing Software — What the Community Actually Wants
### A voice-of-the-people research report · July 2026

**Scope:** What 3D printing users say online about AI *in the slicing workflow* — auto-tuning, orientation, calibration, failure detection — and what they're hostile to. AI model generation is out of scope and only mentioned where it explains the community's baseline temperature toward the word "AI."

**Sources:** Bambu Lab community forum, Prusa forum, OrcaSlicer/PrusaSlicer GitHub issues and discussions, and ~420 scraped YouTube comments (including 92 on a video specifically about an AI-powered OrcaSlicer). Reddit blocks anonymous scraping at the network level, so Reddit threads couldn't be pulled directly — forums and GitHub stand in as the primary community voice. Quotes were adversarially verified against the original threads (verbatim, with reaction counts) wherever possible; caveats are noted inline.

---

## TL;DR

The community's message is remarkably consistent: **"Automate my drudgery, locally, and don't call it AI."**

1. The strongest, oldest demand is **automating tedious expert work** — calibration, orientation, support placement, per-filament tuning. People beg for this without ever using the word "AI."
2. The word "AI" itself is a liability. There is deep hype-fatigue, and shipped "AI" features (Bambu's detection suite) have burned trust with false positives, missed failures, and opacity.
3. Three hard red lines: **must run locally / offline, no account or sign-in, no cloud or vendor lock-in.** The Bambu authorization-control revolt of January 2025 is the defining event — OrcaSlicer's community exists partly *because* of hostility to vendor control.
4. The accepted framing is **assistant, not autopilot**: suggest, explain, and let the user confirm. Anything that silently makes decisions is distrusted.

---

## 1. What people say they WANT an AI-powered slicer to do

### Kill the calibration ritual (the #1 pain-point-shaped wish)

The clearest articulation is on OrcaSlicer's own repo, in [Discussion #701 — "Calibration in the slicer is a great idea - lets make it easier"](https://github.com/SoftFever/OrcaSlicer/discussions/701):

> "The key to this is **automate as much of the process as possible**... and only ask the user to make decisions about print quality."

New users get lost at basic steps ("the instructions say 'Remember to save the filament profile.' As a first time user, I'm wondering, *how?*"), and even a long-time user described a **"love/hate attitude"** toward Orca's calibration, citing terse instructions and unclear reference images.

The tedium is real and measured in the community's own words. From the comments on ModBot's [OrcaSlicer Calibration video](https://youtu.be/g8kNuXuziCc) (156k views):

> "From someone that calibrated **over 20 times the same filament**, I was doing some things wrong or I didn't believed some values..."

> "I keep this on a playlist to reference every few weeks."

And the confusion tax around orderings and interactions, from [the Pressure Advance video](https://youtu.be/EoGdHsU488s) (160k views):

> "what should be tuned first? PA or FLOW?"
> "PA value can be significantly different for different combinations of speed and acceleration... PA must be tested at exact speed AND acceleration for outer walls"
> "Is there an optimal calibration sequence? I tend to do the temp tower first, then pressure advance followed by flow rate."

On the [Bambu forum](https://forum.bambulab.com/t/is-orca-slicer-still-the-recommended-route-to-calibrate-filament/117550), even owners of the "it just works" printer route through OrcaSlicer's manual tests per filament and debate whether the ritual is worth it per spool. And calibration values found in Orca must be **manually re-typed** into Bambu Studio: "manually input the changes in both regardless" ([thread](https://forum.bambulab.com/t/bambu-studio-and-orca-slicer-calibration)). One user's permanent workaround: "the custom filament stuff has been far too unreliable... my workaround for a LONG time now has been to just always leave 'Flow Dynamics Calibration' checked."

**What they're asking for, in product terms:** a guided, end-to-end tuning wizard that runs the right tests in the right order, reads the results (or has the user pick from photos), writes the profile, and syncs it everywhere. Nobody asks for this with the word "AI" — they ask for it with the word "automate."

### Auto-orientation and smarter supports

A years-old, recurring ask. [PrusaSlicer issue #4266](https://github.com/prusa3d/PrusaSlicer/issues/4266) (open since May 2020, comments through 2023):

> "the orientation that reduces the most amount of overhang and needs the least amount supports should be default"
> "It would be awesome if there was an Auto-Rotate button, so you can use minimal support."
> "This would be so useful, especially since this and tree supports are the only things stopping PrusaSlicer from being the best slicer out there imo"
> "Having this done automatically instead of reslicing the object at various angles to find the shortest time would be awesome."

And [issue #13091](https://github.com/prusa3d/PrusaSlicer/issues/13091) (2024) explicitly cites Orca as the benchmark: "It's currently a feature in Orca Slicer." The requester's framing is the interesting part — an explicit *invitation* for the software to out-judge the human:

> "The Slicer/computer should -usually- **know better than me** the best way to orient a model for printing."

### Intelligent structure optimization (the enthusiast's dream feature)

From the comments on YGK3D's ["OrcaSlicer + AI: Intelligent 3D Print Slicing on Autopilot"](https://youtu.be/iZuGebvDBjY) (19k views) — the single most on-topic comment section found:

> "An ai assisted slicer that actually **optimises the structures and print instructions** intelligently would be a game changer. Imagine brick layers and sinewave walls etc being generated by the slicer itself through ai, optimised for that specific print." (6👍)

> "I'm an under-the-hood kinda guy. The majority of my models are functional, so I review every critical layer in my slicer preview. I tweak everything to enhance strength while maintaining appearance. **I would love to see an AI slicer or CAD system that could relieve me of some of that work.** For instance, splitting an object into sub-objects to apply modifiers where needed for strength..." (6👍)

> "AI could be extremely useful for **automating modifiers** when slicing. We'll be able to have it track and lay every line of melted plastic in the best way possible at that moment, not just what works the best for the majority of the model."

> "What about using the AI version of OrcaSlicer for adding modifiers? I was really hoping you would test that." (3👍)

Related, from the Prusa forum: one user wished for **"structural analysis" integrated into slicers** to show where parts might fail before printing ([strategic view thread](https://forum.prusa3d.com/forum/english-forum-general-discussion-announcements-and-releases/a-strategic-view-of-bambu-prusa-including-benchmarks-of-all-flagship-models/paged/4/)).

Note the pattern: the most sophisticated users don't want AI to replace their judgment on *settings* — they want it to do per-region, per-geometry work that's **too tedious for a human to do manually** (modifiers, variable strength, localized parameters).

### Settings prediction from geometry — requested, but with near-zero traction

One concrete feature request exists on Orca's repo ([Discussion #8860, "IA model in Orca Slicer"](https://github.com/SoftFever/OrcaSlicer/discussions/8860), March 2025): train an ML model to "predict optimal printing parameters (such as layer height, print speed, and temperature) based on the features of the 3D model," plus mesh repair and path optimization. **Honest caveat from verification:** it received 2 upvotes, 0 comments, appears partly LLM-written, and drew no maintainer response. One user filed it; the community shrugged. Cite it as an existing idea, not as demand.

### Failure monitoring — wanted, but as a printer/camera feature

Users want their slicer to *integrate* existing vendor AI hardware rather than be locked out of it: [OrcaSlicer Discussion #3125](https://github.com/OrcaSlicer/OrcaSlicer/discussions/3125) asks Orca to expose the Creality K1 Max's built-in AI camera/lidar calibration and failure detection. One Prusa forum user suggested AI print monitoring (video stream watching for detached models, layer shifts) "could be useful" — though verification showed that was a single hedged comment, not a thread consensus.

---

## 2. What they're skeptical or hostile about

### The word "AI" itself — hype fatigue is the baseline

A Prusa forum thread is literally titled ["Is AI in 3D printing actually useful – or just hype?"](https://forum.prusa3d.com/forum/english-forum-general-discussion-announcements-and-releases/is-ai-in-3d-printing-actually-useful-or-just-hype/) The OP:

> "Has anyone found specific ways where AI tools like ChatGPT genuinely save time or improve your workflow? I'm especially interested in **real, practical cases rather than just 'AI is cool' hype**."

The replies are dominated by skepticism: "There is absolutely nothing of ai in the things we have now, its just marketing." The community's precision standard is best captured by a line from a parallel thread:

> "a 3D structure either stands or it doesn't, mechanical components either mesh or they don't. **They need to be right, not right-ish.**"

From the OrcaSlicer+AI YouTube comments, the same fatigue aimed squarely at slicers:

> "i find it really annoying that people forcefully try to stuff AI into everything. Does the AI bot in the slicer produce results? Most likely yes, **but so does picking a pre-made print profile and enabling supports when the slicer tells you**... the AI changing to a draft profile and just increasing the Infill after you told it you wanted a stronger part isn't even the best/most effective way..." (2👍)

That last comment is a warning shot for any AI feature that's a thin wrapper over existing profile switching: users will notice.

### LLMs giving wrong answers about printing

Users have already tried ChatGPT for slicer tuning and troubleshooting, and the experienced crowd's verdict is brutal ([Prusa thread](https://forum.prusa3d.com/forum/general-discussion-announcements-and-releases/using-ai-tools-like-chatgpt-to-optimize-slicing-or-troubleshoot-prints/)):

> "answers which diagnosed the problems were **totally incorrect and misleading**... [beginners] will learn incorrect things and will get bad habits"

> "using gen AI to troubleshoot has been not so helpful, mainly **using more time than learning**... it's better to rely on your own organic intelligence"

What *did* stick as genuinely useful: quick reference lookup — "definitions of GCODEs, or OpenSCAD language questions." The accepted role, in one user's words, is a **"private teacher,"** and the thread consensus is "assistive tool with human oversight, not autonomous."

### Cloud, accounts, and lock-in — the red lines

The top-voted comment on the OrcaSlicer+AI video (17👍):

> "Having a sign in and the model not running locally on my machine 100% offline is a **hell no, burn it with holy water and fire.**"

Second-most-liked sentiment in the same section (5👍): "If the AI tools are running local on MY computer im happy with it and will try / use it!"

This isn't abstract preference — it's scar tissue from the **January–March 2025 Bambu authorization-control revolt**, the single loudest community event in this space. All quotes verified verbatim with reaction counts:

- [OrcaSlicer issue #8063](https://github.com/SoftFever/OrcaSlicer/issues/8063) (283 comments): "**This is NOT about any of the above. This is about lock-in and control.**" (ziehmon, +22) · "the security argument for LAN mode makes no sense... they're deliberately crippling LAN mode as well" (hugheaves, +15) · "Better to just stop recommending their printers to your friends" (+22)
- SoftFever, in the same thread: "I heard back from their development team; they are not going to greenlight OrcaSlicer to send prints directly to their machine. It has to be done through their Bambu Connect application." (Bambu partially walked this back days later with opt-in "Developer Mode.")
- [Bambu forum, firmware 01.08.05.00 thread](https://forum.bambulab.com/t/firmware-01-08-05-00-authorization-control-is-here/152239): "I guess this is now over and I have to choose **Orca or Convenience!?**" · "The bambu connect is a bloatware... designed to limit control" · "This firmware version isn't fit for production - roll it back."
- [Tom's Hardware comment thread](https://www.tomshardware.com/3d-printing/bambu-lab-security-update-will-remove-orcaslicers-access): "Yeah this is just **rent seeking**. The giveaway is the LAN blocking."
- Subscription dread predates the revolt: "I hope 2024 is not the year for Bambu's subscription model" ([Prusa forum](https://forum.prusa3d.com/forum/english-forum-general-discussion-announcements-and-releases/a-strategic-view-of-bambu-prusa-including-benchmarks-of-all-flagship-models/paged/4/)).
- Privacy specifically: users trying to [escape the Bambu cloud](https://forum.bambulab.com/t/understanding-limitations-of-lan-mode-getting-away-from-bbl-cloud/140005) are "worried about Bambu's level of visibility into their printer usage": "Their claim is these security measures are for US — until they change this lie I won't trust them."

One thread title says it all: ["Orca Slicer or die!"](https://forum.bambulab.com/t/orca-slicer-or-die/135872) — a user saying they'd sell their printer if forced off the open slicer.

**Implication:** any AI feature that requires an account, phones home, uploads models, or gates functionality behind a vendor service will be read through this lens, instantly and loudly. A cloud AI slicing feature doesn't start at neutral; it starts at hostile.

### Baseline hostility to AI-branded content (context)

Printables users demanded and got an "exclude AI generated" filter, calling AI content "junk" and "soulless slop" ([thread](https://forum.prusa3d.com/forum/english-forum-general-discussion-announcements-and-releases/printables-filter-out-ai-models/)). That's about model repositories, not slicers — but it calibrates how much goodwill the "AI" label carries in this community: **negative by default, earned back only by utility.** (Counter-voices exist — a 64-year-old user with blood cancer defends AI as an accessibility tool — but they're the minority register.)

---

## 3. Reactions to shipped AI features — the trust ledger

### Bambu's AI detection suite (spaghetti detection, foreign-object detection)

The most instructive dataset, because it's the community grading a real "AI" feature over three years. The verdict is *wildly* mixed, and the failure modes matter more than the average:

**False positives that cost hours:**
> "I get warnings at least every 1-3 prints with a clean and clear build plate" — [P2S owner, "AI detection is a mess"](https://forum.bambulab.com/t/ai-detection-is-a-mess/214801), who concluded "**it's better to disable it completely and hope for the best.**"

> Three false positives in nine attempts on one model ([Cali-Dragon thread](https://forum.bambulab.com/t/frequent-false-spaghetti-positives-on-cali-dragon/7119)), pausing overnight prints.

**Missed real failures:**
> "When it looks like an **Italian restaurant in my printer** it continues making more spaghetti"

> "have yet to have a single AI detection event and have had spaghetti a couple of times" · ABS spaghetti "made its way into the fan ducts" without the AI ever pausing ([X1C thread](https://forum.bambulab.com/t/when-does-the-ai-spaghetti-detection-kick-in/76248)) — known worst case: dark filament, dark plate.

**Opacity — the deepest complaint:**
> "It's AI, meaning **_noone_ knows how/why it gets triggered**"

Plus no feedback channel ("is there a way we can give feedback about false positives to the Bambu team?"), and fear of touching the sensitivity setting at all ("I've never tried changing it incase the High setting is too much and might pause prints it doesn't need to").

**Trust contagion — a bad AI feature poisons the brand:**
> "**if they exaggerated about the AI, what else did they exaggerate about?**"

> "Life is much more enjoyable when you simply ignore everything to do with AI"

**But when it works, people notice and value it:** "3 correct positive hits, but not any false hits, or false misses so far" after 150 hours · "haven't had any false detections so far, 433 hours" · detection "saved my print this morning" · newer hardware "works better than the spaghetti detection on my X1C" ([experiences thread](https://forum.bambulab.com/t/experiences-with-ai-spaghetti-detection/172582)).

### Obico / The Spaghetti Detective

Same shape, smaller scale ([Prusa forum thread](https://forum.prusa3d.com/forum/original-prusa-i3-mk3s-mk3-user-mods-octoprint-enclosures-nozzles/thoughts-on-the-spaghetti-detective/)):

- Cloud privacy discomfort: "**I'm not comfortable with a camera stream inside my home going off to some company**" — self-hosting is the community's preferred answer.
- Hosted tier seen as "pretty expensive and not worth it" unless self-hosted (though contested: "I don't consider it expensive").
- It works once tuned: "You can tweak the sensitivity of the detection, so false positives can be avoided" — becomes "a lifesaver."
- Environmental fragility (lighting) and toolchain conflicts (breaks Octolapse) are real adoption friction.

### The lesson the community keeps teaching

A detection/automation feature is judged on its **worst behavior, not its average**: one 3 a.m. false pause or one melted-spaghetti miss generates the forum thread; 400 quiet hours don't. And every failure is amplified by opacity — users forgive a tunable, explainable system ("tweak the sensitivity") far more readily than a black box ("noone knows how/why it gets triggered").

---

## 4. What this means for building AI into a slicer

Distilled from the voices above, the community's implicit spec:

1. **Local-first is non-negotiable.** Offline, no sign-in, no model uploads. The top-voted comment on the most on-topic video is "hell no, burn it with holy water" for cloud sign-in — and the Bambu revolt shows what happens to vendors who ignore this. If a heavier model needs more compute, the accepted answer is *user-supplied* (one commenter proudly runs local LLMs on a 512GB workstation, feeding it "3D printing elements" via RAG and "getting better results for both models and printer settings").
2. **Automate the drudgery people already beg to have automated:** calibration wizard, filament profile generation, auto-orientation, per-region modifiers/support tuning. This is where demand is proven, years-old, and articulated *without* AI hype attached.
3. **Assistant, not autopilot.** Suggest → explain → confirm. "Imagine them as Junior-Developers in your team... you always have to double check their work" (YouTube, 3👍). Silent parameter changes will be treated as a bug.
4. **Transparency beats accuracy.** Show *why* a suggestion was made and let users tune/override thresholds. The single most corrosive property of Bambu's AI is that nobody can see why it triggers.
5. **Be careful with the label.** "AI" earns eye-rolls; "automatic calibration," "smart orientation," "print check" describe the same features in the language the community itself uses when asking for them. Don't ship a thin wrapper over profile-switching and call it AI — users spot it immediately.
6. **Never degrade the manual path.** This community reveres control ("I review every critical layer in my slicer preview"). AI must be additive and fully ignorable — the users who'd evangelize an Orca AI feature are the same ones who'd fork the project over lock-in.

---

## Source index

**Forums / GitHub (verified primary threads)**
- OrcaSlicer [Discussion #701 — calibration automation ask](https://github.com/SoftFever/OrcaSlicer/discussions/701) · [Discussion #8860 — ML settings prediction (low traction)](https://github.com/SoftFever/OrcaSlicer/discussions/8860) · [Discussion #3125 — K1 Max AI camera integration](https://github.com/OrcaSlicer/OrcaSlicer/discussions/3125) · [Issue #8063 — Bambu firmware lock-in revolt](https://github.com/SoftFever/OrcaSlicer/issues/8063)
- PrusaSlicer [Issue #4266 — auto-orient (2020, recurring)](https://github.com/prusa3d/PrusaSlicer/issues/4266) · [Issue #13091 — auto-orient citing Orca](https://github.com/prusa3d/PrusaSlicer/issues/13091)
- Bambu forum: ["AI detection is a mess"](https://forum.bambulab.com/t/ai-detection-is-a-mess/214801) · [Experiences with AI spaghetti detection](https://forum.bambulab.com/t/experiences-with-ai-spaghetti-detection/172582) · [False positives on Cali-Dragon](https://forum.bambulab.com/t/frequent-false-spaghetti-positives-on-cali-dragon/7119) · [When does detection kick in](https://forum.bambulab.com/t/when-does-the-ai-spaghetti-detection-kick-in/76248) · [Firmware authorization control](https://forum.bambulab.com/t/firmware-01-08-05-00-authorization-control-is-here/152239) · [Escaping the BBL cloud](https://forum.bambulab.com/t/understanding-limitations-of-lan-mode-getting-away-from-bbl-cloud/140005) · [Orca Slicer or die!](https://forum.bambulab.com/t/orca-slicer-or-die/135872) · [Calibrate via Orca?](https://forum.bambulab.com/t/is-orca-slicer-still-the-recommended-route-to-calibrate-filament/117550)
- Prusa forum: [ChatGPT for slicing/troubleshooting](https://forum.prusa3d.com/forum/general-discussion-announcements-and-releases/using-ai-tools-like-chatgpt-to-optimize-slicing-or-troubleshoot-prints/) · [AI useful or hype?](https://forum.prusa3d.com/forum/english-forum-general-discussion-announcements-and-releases/is-ai-in-3d-printing-actually-useful-or-just-hype/) · [Spaghetti Detective thoughts](https://forum.prusa3d.com/forum/original-prusa-i3-mk3s-mk3-user-mods-octoprint-enclosures-nozzles/thoughts-on-the-spaghetti-detective/) · [Printables AI filter](https://forum.prusa3d.com/forum/english-forum-general-discussion-announcements-and-releases/printables-filter-out-ai-models/) · [Bambu/Prusa strategic view](https://forum.prusa3d.com/forum/english-forum-general-discussion-announcements-and-releases/a-strategic-view-of-bambu-prusa-including-benchmarks-of-all-flagship-models/paged/4/)

**YouTube comment sections (scraped via yt-dlp, sorted by likes)**
- [OrcaSlicer + AI: Intelligent 3D Print Slicing on Autopilot](https://youtu.be/iZuGebvDBjY) (92 comments) · [OrcaSlicer Calibration](https://youtu.be/g8kNuXuziCc) (150) · [Pressure Advance tuning](https://youtu.be/EoGdHsU488s) (128) · [SimplyPrint AI failure detection](https://youtu.be/J7A4hytLXxs) · [OctoPrint + Spaghetti Detective](https://youtu.be/qqoAYZuxRZ4)

**Known gaps:** Reddit threads (r/3Dprinting, r/OrcaSlicer, r/BambuLab) were unreachable — Reddit hard-blocks anonymous access at the network level, including via proxies and headless browsers. Independent verification notes suggest Reddit sentiment during the Bambu revolt matched the forums (a tally in one Bambu forum post counted "270+ Reddit comments with none in favor of the update"). Verification also *refuted* one workflow claim (that "Prusa users identify camera monitoring as the genuinely useful AI application" — it was one user's hedged aside), which is reflected above.
