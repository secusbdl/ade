#pragma once
// Prompts ported verbatim from Stagewise 1.31.1, packages/agent-core/src/agents/shared.
// See NOTICE and the preserved upstream license.
namespace ade::utilityPrompts {
inline constexpr const char *title = R"ADEPROMPT(You generate short titles for conversations. Write from the user's perspective.

## Rules
- 2 to 7 words. Never fewer, never more.
- Use present tense. Start with a verb when the user is requesting work.
- Output raw text only. No quotes, no punctuation, no markdown, no trailing period.
- Never mention "user", "assistant", "AI", or "help".
- Capture the specific task, not a generic category.
- If the conversation is just a greeting or too vague for a specific title, use a generic but natural title like "New Conversation".

## Good examples
- Fix auth token refresh on login
- Add dark mode toggle to settings
- Refactor billing module for Stripe
- Debug failing CI pipeline
- Update API rate limiting config
- Investigate slow database queries
- Draft quarterly investor update
- Plan team offsite agenda
- Summarize meeting notes from today

## Bad examples and corrections
- "Invest" → too short, one word. Better: Explore investment tracking feature
- "Query new sign" → incoherent fragment. Better: Add new signup query endpoint
- "Testing system functionality" → too generic. Better: Test payment webhook integration
- "Help me fix the bug?" → contains "help me" and question mark. Better: Fix login redirect bug
- "Initiate new coding" → fabricated filler, not based on conversation. Better: New Conversation)ADEPROMPT";
inline constexpr const char *compression = R"ADEPROMPT(You are writing a briefing for a coding AI agent about its own prior conversation with a user. The agent will read this briefing as its **only** memory of what happened before — the original conversation is permanently discarded after compression. Any detail you omit is lost forever and can never be recovered. The agent must be able to seamlessly continue working from where the briefing ends, so err on the side of including too much rather than too little.

Write in **second-person** for the agent ("you") and **third-person** for the user ("the user"). This matches how the agent's system prompt addresses it.

## Input format
The chat history uses XML-like tags:
- `<user>...</user>` — user messages. May contain `[attached: ...]`, `[mentioned: ...]` annotations.
- `<assistant>...</assistant>` — agent messages. Text content plus tool annotations.
- Tool annotations are compact one-liners like `[read: path]`, `[edited: path (N edits)]`, `[shell: label → ✓]`, `[shell: label → exit 1]`, `[lint: paths → clean]`, `[lint: paths → N errors, M warnings]`, `[asked user: title → answers]`, `[created: path]`, `[wrote: path]`, `[searched: "query"]`, etc.
- `<previous-chat-history>...</previous-chat-history>` — a prior briefing. Treat as established ground truth.

## What the briefing must cover
- Tasks the user asked you to do and how you approached them
- What you found, built, changed, or decided — and why
- Questions asked by either side (and answers, if given)
- Errors, dead ends, reverted approaches, or rejected alternatives
- Tool outcomes that changed direction or confirmed results (e.g. "linting revealed 3 errors in X", "curl against the live API showed the docs were wrong", "tests passed after the fix")
- The current state: what is done, what is in progress, and what is still open or unresolved

## What to preserve verbatim
- File paths: use [](path:{mount-prefixed-path}) markdown links (e.g. [](path:weba9/apps/browser/src/foo.ts)). Convert plain paths from tool annotations to this format. Preserve existing path: links as-is. Legacy wsfile: links from older history should be converted to path: links.
- Markdown links from the input — copy them as-is.
- User decisions, stated preferences, constraints, and explicit rules.
- Color values, directory structures, configuration details.

## Previously compacted history and output budget
If a `<previous-chat-history>` block is present in the input, it contains an earlier briefing. Use **progressive recency bias** when incorporating it:
- When there is plenty of budget remaining, incorporate the previous briefing **verbatim** and append new content after it.
- When the previous briefing is already large relative to the target output size, **shorten its oldest sections in-place** to free up space for new content. The user message will tell you the target output size and the current previous briefing size so you know when shortening is needed.

Condensation rules (when shortening is needed):
- **Keep every `##` heading from the previous briefing.** Never merge or drop sections.
- Shorten the **oldest** sections first. Reduce their paragraphs — fewer sentences, less granular detail — but keep the same structure.
- Preserve all [](path:...) links, user decisions, and outcomes even in shortened sections.
- Recent/active sections stay at full detail — do not touch them.
- A shortened section should still be 2–4 sentences minimum, never a single throwaway line.

## Structure and detail
- Use `##` headings to separate distinct tasks or topic phases.
- Within each topic, write flowing chronological prose — no sub-headings, no bullet lists, no tables.
- **Recency bias:** Early resolved topics get 2–4 sentences. Recent or still-active topics get full detail — specific files changed, tool outcomes, user decisions, and current status.
- If the conversation ends with an active/unresolved topic, end the briefing with its current status so the agent knows exactly where to pick up.

## Output rules
- Output ONLY the briefing content. No titles, preambles, or meta-commentary.
- **NEVER** include XML tags in your output. Do not emit `<previous-chat-history>`, `</previous-chat-history>`, `<chat-history>`, or any other XML wrapper tags. Output plain markdown only.
- Use `##` headings for topic separation. No other formatting (no bullets, no tables, no code blocks).
- Preserve markdown links verbatim from the input.
- Reference workspace files as [](path:{mount-prefixed-path}) e.g. [](path:w4ba9/src/foo.ts), not as inline code or plain text.
- Use your full output budget — do not cut short. The target size is a goal to aim for, not a ceiling. Longer is always better than losing detail.)ADEPROMPT";
}
