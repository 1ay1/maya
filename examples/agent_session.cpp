// maya — Claude Code-style inline terminal agent session
//
// A single-column vertical flow simulating an AI coding agent working through
// a multi-file JWT refactoring task. No tabs, no sidebar — the terminal's own
// scrollback provides scrolling, just like Claude Code.
//
// Usage: ./maya_agent_session
//        q/Esc to quit, 1-5 toggle tool cards.

#include <maya/maya.hpp>
#include <maya/dsl.hpp>
#include <maya/components/components.hpp>

// Components not yet in the umbrella header
#include <maya/components/context_pill.hpp>
#include <maya/components/glimmer_text.hpp>
#include <maya/components/read_card.hpp>
#include <maya/components/command_card.hpp>
#include <maya/components/accordion_bar.hpp>
#include <maya/components/feedback_buttons.hpp>
#include <maya/components/checkpoint.hpp>
#include <maya/components/edit_card.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <string>
#include <vector>

using namespace maya;
using namespace maya::dsl;
using namespace maya::components;

// ═══════════════════════════════════════════════════════════════════════════════
// Simulated content — fake diffs, code, messages
// ═══════════════════════════════════════════════════════════════════════════════

static const char* kThinkingContent =
    "The user wants to refactor the authentication middleware to use JWT tokens "
    "instead of session cookies. This involves several files:\n\n"
    "1. `src/middleware/auth.ts` — the main auth middleware\n"
    "2. `src/utils/token.ts` — token generation/verification\n"
    "3. `src/routes/login.ts` — the login endpoint\n"
    "4. `src/types/auth.ts` — type definitions\n"
    "5. `tests/auth.test.ts` — test suite\n\n"
    "I need to:\n"
    "- Replace express-session with jsonwebtoken\n"
    "- Add token refresh logic\n"
    "- Update all route handlers that check `req.session`\n"
    "- Migrate the test suite\n"
    "- Make sure the CORS config allows the Authorization header\n\n"
    "Let me start by reading the current auth middleware to understand the "
    "session-based approach, then create the token utility, update the middleware, "
    "and finally fix the tests.";

static const char* kDiff1 =
    "@@ -1,18 +1,24 @@\n"
    "-import session from 'express-session';\n"
    "-import { SessionStore } from '../stores/session';\n"
    "+import jwt from 'jsonwebtoken';\n"
    "+import { TokenPayload, AuthConfig } from '../types/auth';\n"
    " \n"
    "-const SESSION_SECRET = process.env.SESSION_SECRET!;\n"
    "-const SESSION_TIMEOUT = 3600; // seconds\n"
    "+const JWT_SECRET = process.env.JWT_SECRET!;\n"
    "+const TOKEN_EXPIRY = '1h';\n"
    "+const REFRESH_EXPIRY = '7d';\n"
    " \n"
    "-export function authMiddleware() {\n"
    "-  return session({\n"
    "-    secret: SESSION_SECRET,\n"
    "-    store: new SessionStore(),\n"
    "-    resave: false,\n"
    "-    saveUninitialized: false,\n"
    "-    cookie: {\n"
    "-      maxAge: SESSION_TIMEOUT * 1000, // BUG: was just SESSION_TIMEOUT\n"
    "-      httpOnly: true,\n"
    "-      secure: process.env.NODE_ENV === 'production',\n"
    "-    },\n"
    "-  });\n"
    "+export function authMiddleware(config: AuthConfig = {}) {\n"
    "+  return (req: Request, res: Response, next: NextFunction) => {\n"
    "+    const token = extractToken(req);\n"
    "+    if (!token) return res.status(401).json({ error: 'No token provided' });\n"
    "+\n"
    "+    try {\n"
    "+      const payload = jwt.verify(token, JWT_SECRET) as TokenPayload;\n"
    "+      req.user = payload;\n"
    "+      next();\n"
    "+    } catch (err) {\n"
    "+      if (err instanceof jwt.TokenExpiredError) {\n"
    "+        return res.status(401).json({ error: 'Token expired', code: 'TOKEN_EXPIRED' });\n"
    "+      }\n"
    "+      return res.status(403).json({ error: 'Invalid token' });\n"
    "+    }\n"
    "+  };\n"
    " }";

static const char* kDiff2 =
    "@@ -1,5 +1,32 @@\n"
    "+import jwt from 'jsonwebtoken';\n"
    "+import crypto from 'crypto';\n"
    "+import { TokenPayload, TokenPair } from '../types/auth';\n"
    "+\n"
    "+const JWT_SECRET = process.env.JWT_SECRET!;\n"
    "+const REFRESH_SECRET = process.env.REFRESH_SECRET ?? JWT_SECRET;\n"
    "+\n"
    "+export function generateTokens(user: { id: string; role: string }): TokenPair {\n"
    "+  const payload: TokenPayload = {\n"
    "+    sub: user.id,\n"
    "+    role: user.role,\n"
    "+    jti: crypto.randomUUID(),\n"
    "+  };\n"
    "+\n"
    "+  const accessToken = jwt.sign(payload, JWT_SECRET, { expiresIn: '1h' });\n"
    "+  const refreshToken = jwt.sign(\n"
    "+    { sub: user.id, jti: crypto.randomUUID() },\n"
    "+    REFRESH_SECRET,\n"
    "+    { expiresIn: '7d' }\n"
    "+  );\n"
    "+\n"
    "+  return { accessToken, refreshToken };\n"
    "+}\n"
    "+\n"
    "+export function verifyRefreshToken(token: string): { sub: string } {\n"
    "+  return jwt.verify(token, REFRESH_SECRET) as { sub: string };\n"
    "+}\n"
    "+\n"
    "+export function extractToken(req: Request): string | null {\n"
    "+  const header = req.headers.authorization;\n"
    "+  if (header?.startsWith('Bearer ')) return header.slice(7);\n"
    "+  return null;\n"
    "+}";

static const char* kDiff3 =
    "@@ -12,19 +12,26 @@\n"
    " describe('auth middleware', () => {\n"
    "-  let sessionStore: MockSessionStore;\n"
    "+  let testUser: { id: string; role: string };\n"
    "+  let tokens: TokenPair;\n"
    " \n"
    "   beforeEach(() => {\n"
    "-    sessionStore = new MockSessionStore();\n"
    "+    testUser = { id: 'user-123', role: 'admin' };\n"
    "+    tokens = generateTokens(testUser);\n"
    "   });\n"
    " \n"
    "-  it('should reject expired sessions', async () => {\n"
    "-    const req = mockRequest({ session: { expired: true } });\n"
    "+  it('should reject expired tokens', async () => {\n"
    "+    const expiredToken = jwt.sign(\n"
    "+      { sub: 'user-123', role: 'admin' },\n"
    "+      process.env.JWT_SECRET!,\n"
    "+      { expiresIn: '0s' }\n"
    "+    );\n"
    "+    const req = mockRequest({ authorization: `Bearer ${expiredToken}` });\n"
    "     const res = mockResponse();\n"
    "     await authMiddleware()(req, res, next);\n"
    "-    expect(res.status).toHaveBeenCalledWith(401);\n"
    "+    expect(res.status).toHaveBeenCalledWith(401);\n"
    "+    expect(res.json).toHaveBeenCalledWith(\n"
    "+      expect.objectContaining({ code: 'TOKEN_EXPIRED' })\n"
    "+    );\n"
    "   });\n"
    " \n"
    "-  it('should attach user to request', async () => {\n"
    "-    const req = mockRequest({ session: { userId: '123' } });\n"
    "+  it('should attach decoded payload to request', async () => {\n"
    "+    const req = mockRequest({ authorization: `Bearer ${tokens.accessToken}` });\n"
    "     const res = mockResponse();\n"
    "     await authMiddleware()(req, res, next);\n"
    "-    expect(req.user).toEqual({ id: '123' });\n"
    "+    expect(req.user).toMatchObject({ sub: 'user-123', role: 'admin' });\n"
    "   });";

static const char* kCodePreview =
    "import jwt from 'jsonwebtoken';\n"
    "import { TokenPayload, AuthConfig } from '../types/auth';\n"
    "\n"
    "const JWT_SECRET = process.env.JWT_SECRET!;\n"
    "const TOKEN_EXPIRY = '1h';\n"
    "const REFRESH_EXPIRY = '7d';\n"
    "\n"
    "export function authMiddleware(config: AuthConfig = {}) {\n"
    "  return (req: Request, res: Response, next: NextFunction) => {\n"
    "    const token = extractToken(req);\n"
    "    if (!token) return res.status(401).json({ error: 'No token provided' });\n"
    "\n"
    "    try {\n"
    "      const payload = jwt.verify(token, JWT_SECRET) as TokenPayload;\n"
    "      req.user = payload;\n"
    "      next();\n"
    "    } catch (err) {\n"
    "      if (err instanceof jwt.TokenExpiredError) {\n"
    "        return res.status(401).json({\n"
    "          error: 'Token expired',\n"
    "          code: 'TOKEN_EXPIRED',\n"
    "        });\n"
    "      }\n"
    "      return res.status(403).json({ error: 'Invalid token' });\n"
    "    }\n"
    "  };\n"
    "}";

static const char* kAssistantResponse =
    "I've refactored the authentication system from session-based cookies to JWT tokens. Here's what changed:\n"
    "\n"
    "## Changes\n"
    "\n"
    "**1. `src/middleware/auth.ts`** — Replaced `express-session` with `jsonwebtoken`. "
    "The middleware now extracts the Bearer token from the `Authorization` header, verifies it, "
    "and attaches the decoded payload to `req.user`. It returns specific error codes for expired vs invalid tokens.\n"
    "\n"
    "**2. `src/utils/token.ts`** *(new file)* — Token utility module:\n"
    "\n"
    "- `generateTokens()` — creates an access/refresh token pair with `crypto.randomUUID()` for jti\n"
    "- `verifyRefreshToken()` — validates refresh tokens using a separate secret\n"
    "- `extractToken()` — parses the `Authorization: Bearer <token>` header\n"
    "\n"
    "**3. `tests/auth.test.ts`** — Migrated the test suite from mock sessions to JWT tokens. All **14 tests** pass across 3 suites.\n"
    "\n"
    "## Root cause\n"
    "\n"
    "> The old session-cookie approach had a subtle bug where `SESSION_TIMEOUT` was in seconds but `cookie.maxAge` expected milliseconds — sessions expired *1000x too quickly*.\n"
    "\n"
    "The JWT approach avoids this entirely since expiry is encoded in the token itself via the `exp` claim. The token utility generates tokens like:\n"
    "\n"
    "```typescript\n"
    "const accessToken = jwt.sign(payload, JWT_SECRET, { expiresIn: '1h' });\n"
    "```\n"
    "\n"
    "No more unit mismatches — the `jsonwebtoken` library handles expiry validation internally.\n";

// Test output lines
static const char* kTestLines[] = {
    "\033[32m  PASS\033[0m  tests/auth.test.ts",
    "\033[32m    ✓\033[0m should reject requests without tokens \033[90m(3ms)\033[0m",
    "\033[32m    ✓\033[0m should reject expired tokens \033[90m(12ms)\033[0m",
    "\033[32m    ✓\033[0m should reject malformed tokens \033[90m(2ms)\033[0m",
    "\033[32m    ✓\033[0m should attach decoded payload to request \033[90m(5ms)\033[0m",
    "\033[32m    ✓\033[0m should handle token refresh flow \033[90m(18ms)\033[0m",
    "",
    "\033[32m  PASS\033[0m  tests/token.test.ts",
    "\033[32m    ✓\033[0m should generate valid access token \033[90m(4ms)\033[0m",
    "\033[32m    ✓\033[0m should generate valid refresh token \033[90m(3ms)\033[0m",
    "\033[32m    ✓\033[0m should reject expired refresh tokens \033[90m(8ms)\033[0m",
    "\033[32m    ✓\033[0m should extract Bearer token from header \033[90m(1ms)\033[0m",
    "\033[32m    ✓\033[0m should return null for missing header \033[90m(1ms)\033[0m",
    "",
    "\033[32m  PASS\033[0m  tests/routes/login.test.ts",
    "\033[32m    ✓\033[0m should return token pair on valid login \033[90m(45ms)\033[0m",
    "\033[32m    ✓\033[0m should reject invalid credentials \033[90m(12ms)\033[0m",
    "\033[32m    ✓\033[0m should refresh tokens with valid refresh token \033[90m(8ms)\033[0m",
    "\033[32m    ✓\033[0m should reject blacklisted refresh tokens \033[90m(6ms)\033[0m",
    "",
    "\033[1mTest Suites: \033[32m3 passed\033[0m, 3 total",
    "\033[1mTests:       \033[32m14 passed\033[0m, 14 total",
    "\033[1mSnapshots:   0 total",
    "\033[1mTime:        1.847s",
    "\033[90mRan all test suites matching /auth/.\033[0m",
};
static constexpr int kNumTestLines = sizeof(kTestLines) / sizeof(kTestLines[0]);

// Server log lines (background noise)
static const char* kServerLogs[] = {
    "\033[34m[08:31:02]\033[0m \033[90mGET\033[0m  /api/health \033[32m200\033[0m 2ms",
    "\033[34m[08:31:05]\033[0m \033[90mPOST\033[0m /api/auth/login \033[32m200\033[0m 45ms",
    "\033[34m[08:31:06]\033[0m \033[90mGET\033[0m  /api/users/me \033[32m200\033[0m 12ms",
    "\033[34m[08:31:08]\033[0m \033[33m[WARN]\033[0m  Token refresh rate elevated: 23 req/min",
    "\033[34m[08:31:10]\033[0m \033[90mGET\033[0m  /api/projects \033[32m200\033[0m 8ms",
    "\033[34m[08:31:12]\033[0m \033[90mPUT\033[0m  /api/users/42 \033[32m200\033[0m 18ms",
    "\033[34m[08:31:14]\033[0m \033[90mGET\033[0m  /api/health \033[32m200\033[0m 1ms",
    "\033[34m[08:31:15]\033[0m \033[31m[ERROR]\033[0m Connection pool exhausted, retrying...",
    "\033[34m[08:31:16]\033[0m \033[32m[INFO]\033[0m  Pool recovered: 12/50 connections active",
    "\033[34m[08:31:18]\033[0m \033[90mPOST\033[0m /api/auth/refresh \033[32m200\033[0m 23ms",
    "\033[34m[08:31:20]\033[0m \033[90mDELETE\033[0m /api/sessions/old \033[32m204\033[0m 156ms",
    "\033[34m[08:31:22]\033[0m \033[90mGET\033[0m  /api/metrics \033[32m200\033[0m 4ms",
    "\033[34m[08:31:25]\033[0m \033[34m[DEBUG]\033[0m GC pause: 8ms, heap: 189MB/512MB",
    "\033[34m[08:31:28]\033[0m \033[90mGET\033[0m  /api/health \033[32m200\033[0m 1ms",
    "\033[34m[08:31:30]\033[0m \033[32m[INFO]\033[0m  Cache hit rate: 94.2% (last 60s)",
};
static constexpr int kNumServerLogs = sizeof(kServerLogs) / sizeof(kServerLogs[0]);

// ═══════════════════════════════════════════════════════════════════════════════
// Application state
// ═══════════════════════════════════════════════════════════════════════════════

// The simulation progresses through these phases
enum class Phase {
    UserMessage,        // Show user's request
    Thinking,           // Agent is reasoning (streaming thinking block)
    ReadFile,           // Tool call: Read src/middleware/auth.ts
    EditAuth,           // Tool call: Edit auth middleware
    CreateToken,        // Tool call: Create token utility
    EditTests,          // Tool call: Edit test suite
    RunTests,           // Tool call: Run test suite (streaming log output)
    Responding,         // Agent streams its final response
    Complete,           // Session complete — all done
};

struct State {
    int frame = 0;
    float elapsed = 0.f;

    // ── Phase progression ──────────────────────────────────────
    Phase phase = Phase::UserMessage;
    float phase_timer = 0.f;
    bool phase_just_entered = true;

    // ── Thinking ──────────────────────────────────────────────
    ThinkingBlock thinking{ThinkingBlockProps{
        .content = "",
        .expanded = true,
        .is_streaming = true,
    }};
    StreamingText thinking_stream{StreamingTextProps{
        .text = kThinkingContent, .show_cursor = false,
    }};
    int thinking_chars = 0;

    // ── Response streaming ────────────────────────────────────
    StreamingText response_stream{StreamingTextProps{
        .text = kAssistantResponse, .show_cursor = false,
    }};
    bool response_started = false;

    // ── Tool call states ──────────────────────────────────────
    TaskStatus read_status    = TaskStatus::Pending;
    TaskStatus edit1_status   = TaskStatus::Pending;
    TaskStatus create_status  = TaskStatus::Pending;
    TaskStatus edit2_status   = TaskStatus::Pending;
    TaskStatus test_status    = TaskStatus::Pending;

    bool tool1_collapsed = false;
    bool tool2_collapsed = false;
    bool tool3_collapsed = false;
    bool tool4_collapsed = false;
    bool tool5_collapsed = false;

    // ── Read card ─────────────────────────────────────────────
    ReadCard read_card{ReadCardProps{
        .file_path = "src/middleware/auth.ts",
        .content = "import session from 'express-session';\n"
                   "import { SessionStore } from '../stores/session';\n"
                   "\n"
                   "const SESSION_SECRET = process.env.SESSION_SECRET!;\n"
                   "const SESSION_TIMEOUT = 3600; // seconds\n"
                   "\n"
                   "export function authMiddleware() {\n"
                   "  return session({\n"
                   "    secret: SESSION_SECRET,\n"
                   "    // ... (42 lines)\n"
                   "  });\n"
                   "}",
        .line_count = 42,
        .status = TaskStatus::Pending,
        .collapsed = false,
        .language = "typescript",
    }};

    // ── Command card ──────────────────────────────────────────
    CommandCard cmd_card{CommandCardProps{
        .command = "npx vitest run --filter auth",
        .status = TaskStatus::Pending,
        .collapsed = false,
    }};

    // ── Accordion (file changes summary) ──────────────────────
    AccordionBar accordion{AccordionBarProps{.show_keyhints = false}};
    bool show_accordion = false;

    // ── Test output ───────────────────────────────────────────
    LogView test_log{LogViewProps{
        .max_visible = 16,
        .tail_follow = true,
        .show_line_nums = true,
    }};
    int test_line = 0;
    float test_timer = 0.f;

    // ── Feedback ──────────────────────────────────────────────
    FeedbackButtons feedback{FeedbackButtonsProps{}};

    // ── Checkpoint ────────────────────────────────────────────
    Checkpoint checkpoint{CheckpointProps{
        .label = "Before JWT refactor",
        .file_count = 3,
        .added = 65,
        .removed = 22,
        .timestamp = "just now",
    }};

    // ── Toast ─────────────────────────────────────────────────
    ToastStack toasts{ToastStackProps{.max_visible = 3, .show_timer = true}};
    bool toast_pushed = false;

    // ── Disclosure (detailed stats) ───────────────────────────
    Disclosure stats_disclosure{DisclosureProps{
        .title = "Detailed Stats",
        .expanded = false,
        .badge = "6 metrics",
    }};

    // ── Approve edit toggle ───────────────────────────────────
    bool edit_approved = true;

    // ── Cost tracking ─────────────────────────────────────────
    int total_input_tokens = 0;
    int total_output_tokens = 0;
    int cache_read = 0;
    int cache_write = 0;
    double total_cost = 0.0;

    // ── Token rate tracking ───────────────────────────────────
    std::vector<float> tok_rate_history;
    float tok_rate_timer = 0.f;
    float peak_tok_rate = 0.f;
    int   prev_output_tokens = 0;

    // ── Heatmap data (token activity) ─────────────────────────
    std::vector<std::vector<float>> heatmap_data;
    float heatmap_timer = 0.f;
    int heatmap_col = 0;
};

// ═══════════════════════════════════════════════════════════════════════════════
// Phase progression logic
// ═══════════════════════════════════════════════════════════════════════════════

static void enter_phase(State& st, Phase p) {
    st.phase = p;
    st.phase_timer = 0.f;
    st.phase_just_entered = true;
}

static void advance_phase(State& st, float dt) {
    st.phase_timer += dt;
    bool just = st.phase_just_entered;
    st.phase_just_entered = false;

    switch (st.phase) {
    case Phase::UserMessage:
        if (st.phase_timer > 1.5f) {
            st.total_input_tokens += 847;
            st.cache_read += 2400;
            enter_phase(st, Phase::Thinking);
        }
        break;

    case Phase::Thinking:
        // Stream thinking content
        if (st.frame % 2 == 0 && !st.thinking_stream.fully_revealed()) {
            st.thinking_stream.advance(3);
            st.thinking_chars += 3;
            st.thinking.set_content(
                std::string(kThinkingContent, 0,
                    std::min<size_t>(st.thinking_chars, std::string_view(kThinkingContent).size())));
        }
        if (st.thinking_stream.fully_revealed() && st.phase_timer > 4.f) {
            st.thinking.set_streaming(false);
            st.total_output_tokens += 420;
            st.total_cost += 0.063;
            enter_phase(st, Phase::ReadFile);
        }
        break;

    case Phase::ReadFile:
        if (just) {
            st.read_status = TaskStatus::InProgress;
            st.read_card.set_status(TaskStatus::InProgress);
        }
        if (st.phase_timer > 1.2f) {
            st.read_status = TaskStatus::Completed;
            st.read_card.set_status(TaskStatus::Completed);
            st.total_input_tokens += 3200;
            st.cache_write += 1200;
            st.total_cost += 0.048;
            enter_phase(st, Phase::EditAuth);
        }
        break;

    case Phase::EditAuth:
        if (just) {
            st.edit1_status = TaskStatus::InProgress;
        }
        if (st.phase_timer > 2.0f) {
            st.edit1_status = TaskStatus::Completed;
            st.total_output_tokens += 1840;
            st.total_cost += 0.138;
            st.accordion.add_file({.path = "src/middleware/auth.ts", .added = 18, .removed = 14});
            enter_phase(st, Phase::CreateToken);
        }
        break;

    case Phase::CreateToken:
        if (just) {
            st.create_status = TaskStatus::InProgress;
        }
        if (st.phase_timer > 2.0f) {
            st.create_status = TaskStatus::Completed;
            st.total_output_tokens += 2100;
            st.total_cost += 0.158;
            st.accordion.add_file({.path = "src/utils/token.ts", .added = 32, .is_new = true});
            enter_phase(st, Phase::EditTests);
        }
        break;

    case Phase::EditTests:
        if (just) {
            st.edit2_status = TaskStatus::InProgress;
        }
        if (st.phase_timer > 2.0f) {
            st.edit2_status = TaskStatus::Completed;
            st.total_output_tokens += 1650;
            st.total_cost += 0.124;
            st.accordion.add_file({.path = "tests/auth.test.ts", .added = 15, .removed = 8});
            st.show_accordion = true;
            enter_phase(st, Phase::RunTests);
        }
        break;

    case Phase::RunTests:
        if (just) {
            st.test_status = TaskStatus::InProgress;
            st.test_line = 0;
            st.test_timer = 0.f;
            st.cmd_card.set_status(TaskStatus::InProgress);
        }
        // Stream test output
        st.test_timer += dt;
        if (st.test_timer > 0.12f && st.test_line < kNumTestLines) {
            st.test_timer = 0.f;
            st.test_log.append(kTestLines[st.test_line]);
            // Build output string for CommandCard
            std::string output;
            for (int i = 0; i <= st.test_line; i++) {
                if (i > 0) output += "\n";
                output += kTestLines[i];
            }
            st.cmd_card.set_output(output);
            st.test_line++;
        }
        if (st.test_line >= kNumTestLines && st.phase_timer > 4.f) {
            st.test_status = TaskStatus::Completed;
            st.cmd_card.set_status(TaskStatus::Completed);
            st.cmd_card.set_exit_code(0);
            st.total_input_tokens += 1200;
            st.total_cost += 0.018;
            enter_phase(st, Phase::Responding);
        }
        break;

    case Phase::Responding:
        if (just) st.response_started = true;
        if (st.frame % 2 == 0 && !st.response_stream.fully_revealed()) {
            st.response_stream.advance(4);
            st.total_output_tokens += 4;
        }
        if (st.response_stream.fully_revealed()) {
            st.total_cost += 0.089;
            enter_phase(st, Phase::Complete);
        }
        break;

    case Phase::Complete:
        // Push toast once on completion
        if (!st.toast_pushed) {
            st.toasts.push("All 14 tests passing — task complete", Severity::Success, 6.0f);
            st.toast_pushed = true;
        }
        break;
    }
}

static void advance_metrics(State& st, float dt) {
    // Token rate tracking
    st.tok_rate_timer += dt;
    if (st.tok_rate_timer > 0.3f) {
        st.tok_rate_timer = 0.f;
        int delta = st.total_output_tokens - st.prev_output_tokens;
        float rate = static_cast<float>(delta) / 0.3f;
        st.prev_output_tokens = st.total_output_tokens;
        st.tok_rate_history.push_back(rate);
        if (rate > st.peak_tok_rate) st.peak_tok_rate = rate;
        auto trim = [](std::vector<float>& v, size_t max) {
            if (v.size() > max) v.erase(v.begin());
        };
        trim(st.tok_rate_history, 40);
    }

    // Heatmap data: build a 4-row x N-col grid of token activity
    st.heatmap_timer += dt;
    if (st.heatmap_timer > 0.5f) {
        st.heatmap_timer = 0.f;
        // Ensure 4 rows exist
        if (st.heatmap_data.size() < 4) st.heatmap_data.resize(4);
        float t = st.elapsed;
        float base = (st.phase >= Phase::Thinking && st.phase <= Phase::Responding) ? 0.3f : 0.05f;
        st.heatmap_data[0].push_back(std::clamp(base + 0.3f * std::sin(t * 1.1f), 0.f, 1.f));
        st.heatmap_data[1].push_back(std::clamp(base + 0.2f * std::cos(t * 0.7f), 0.f, 1.f));
        st.heatmap_data[2].push_back(std::clamp(base + 0.4f * std::sin(t * 1.5f) * std::sin(t * 0.3f), 0.f, 1.f));
        st.heatmap_data[3].push_back(std::clamp(base + 0.25f * std::cos(t * 2.1f), 0.f, 1.f));
        // Keep last 20 columns
        for (auto& row : st.heatmap_data) {
            if (row.size() > 20) row.erase(row.begin());
        }
        st.heatmap_col++;
    }

    // Tick toasts
    st.toasts.tick(dt);
}

static void advance(State& st, float dt) {
    st.frame++;
    st.elapsed += dt;
    advance_phase(st, dt);
    advance_metrics(st, dt);
}

// ═══════════════════════════════════════════════════════════════════════════════
// Helpers
// ═══════════════════════════════════════════════════════════════════════════════

static Element muted(const std::string& s) {
    return text(s, Style{}.with_fg(palette().muted).with_dim());
}

// ═══════════════════════════════════════════════════════════════════════════════
// Render — single-column vertical flow
// ═══════════════════════════════════════════════════════════════════════════════

static Element build_ui(State& st) {
    auto& p = palette();
    auto phase = st.phase;
    std::vector<Element> rows;

    // ── User message with context pills ────────────────────────
    rows.push_back(
        MessageBubble({
            .role = Role::User,
            .content = "Refactor the auth middleware to use JWT tokens instead of "
                       "session cookies. The current session-based approach has a "
                       "timeout bug and doesn't scale across our load-balanced servers. "
                       "Update the tests too.",
            .timestamp = "08:30:42",
        })
    );

    rows.push_back(ContextPillRow({.pills = {
        {.kind = ContextKind::File,      .label = "src/middleware/auth.ts"},
        {.kind = ContextKind::File,      .label = "tests/auth.test.ts"},
        {.kind = ContextKind::GitDiff,   .label = "Working Changes"},
        {.kind = ContextKind::Directory, .label = "src/utils/"},
    }}));

    // ── Thinking block with GlimmerText label ─────────────────
    if (phase >= Phase::Thinking) {
        bool thinking_active = phase == Phase::Thinking;
        Children thinking_children;
        if (thinking_active) {
            thinking_children.push_back(
                GlimmerText({.text_content = "Thinking...",
                             .frame = st.frame, .is_streaming = true}));
        }
        thinking_children.push_back(st.thinking.render(st.frame));
        rows.push_back(
            MessageBubble({
                .role = Role::Assistant,
                .children = std::move(thinking_children),
                .is_streaming = thinking_active,
                .frame = st.frame,
            })
        );
    }

    // ── Tool 1: Read file (ReadCard) ──────────────────────────
    if (phase >= Phase::ReadFile) {
        rows.push_back(st.read_card.render(st.frame));
    }

    // ── Tool 2: Edit auth middleware (ToolCard + DiffView) ────
    if (phase >= Phase::EditAuth) {
        if (phase == Phase::EditAuth && st.phase_timer < 0.5f) {
            rows.push_back(hstack().gap(2)(
                Toggle({.checked = st.edit_approved, .label = "Auto-approve edits",
                        .style = ToggleStyle::Switch}),
                muted("press 'a' to toggle")
            ));
        }

        Children edit1_body;
        edit1_body.push_back(
            FileBreadcrumb("src/middleware/auth.ts", p.accent));
        edit1_body.push_back(
            DiffView({.diff = kDiff1, .file_path = "src/middleware/auth.ts"}));
        rows.push_back(ToolCard({
            .title = "Edit src/middleware/auth.ts",
            .status = st.edit1_status,
            .frame = st.frame,
            .collapsed = false,
            .summary = st.edit1_status == TaskStatus::Completed ? "+18 -14 lines" : "",
            .children = std::move(edit1_body),
            .tool_name = "Edit",
        }));
    }

    // ── Tool 3: Create token utility (ToolCard + DiffView) ────
    if (phase >= Phase::CreateToken) {
        Children create_body;
        create_body.push_back(
            FileBreadcrumb("src/utils/token.ts", p.success));
        create_body.push_back(
            DiffView({.diff = kDiff2, .file_path = "src/utils/token.ts"}));
        rows.push_back(ToolCard({
            .title = "Write src/utils/token.ts",
            .status = st.create_status,
            .frame = st.frame,
            .collapsed = false,
            .summary = st.create_status == TaskStatus::Completed ? "+32 (new file)" : "",
            .children = std::move(create_body),
            .tool_name = "Write",
        }));
    }

    // ── Tool 4: Edit tests (ToolCard + DiffView) ──────────────
    if (phase >= Phase::EditTests) {
        Children edit2_body;
        edit2_body.push_back(
            FileBreadcrumb("tests/auth.test.ts", p.accent));
        edit2_body.push_back(
            DiffView({.diff = kDiff3, .file_path = "tests/auth.test.ts"}));
        rows.push_back(ToolCard({
            .title = "Edit tests/auth.test.ts",
            .status = st.edit2_status,
            .frame = st.frame,
            .collapsed = false,
            .summary = st.edit2_status == TaskStatus::Completed ? "+15 -8 lines" : "",
            .children = std::move(edit2_body),
            .tool_name = "Edit",
        }));

        if (st.show_accordion) {
            rows.push_back(st.accordion.render());
        }
    }

    // ── Timeline + Test execution ─────────────────────────────
    if (phase >= Phase::RunTests) {
        bool tests_done = phase > Phase::RunTests;
        rows.push_back(Timeline({
            .events = {
                {.label = "Read auth middleware", .detail = "42 lines",
                 .duration = "1.2s", .status = TaskStatus::Completed},
                {.label = "Edit auth.ts", .detail = "+18 -14",
                 .duration = "2.0s", .status = TaskStatus::Completed},
                {.label = "Create token.ts", .detail = "+32 new",
                 .duration = "2.0s", .status = TaskStatus::Completed},
                {.label = "Edit tests", .detail = "+15 -8",
                 .duration = "2.0s", .status = TaskStatus::Completed},
                {.label = "Run tests", .detail = tests_done ? "14 passed" : "running...",
                 .duration = tests_done ? "1.8s" : "",
                 .status = tests_done ? TaskStatus::Completed : TaskStatus::InProgress,
                 .bar_width = tests_done ? 12 : 8},
            },
            .compact = true,
            .frame = st.frame,
        }));

        rows.push_back(st.cmd_card.render(st.frame));

        float test_progress = 0.f;
        if (phase > Phase::RunTests) test_progress = 1.f;
        else test_progress = static_cast<float>(st.test_line) / kNumTestLines;

        bool tp_done = test_progress >= 1.f;
        rows.push_back(hstack().gap(2)(
            muted("progress"),
            ProgressBar({
                .value = test_progress,
                .width = 30,
                .show_percent = true,
                .filled = tp_done ? p.success : p.primary,
            })
        ));
    }

    // ── Assistant response (Markdown rendering) ────────────────
    if (phase >= Phase::Responding) {
        bool still_streaming = !st.response_stream.fully_revealed();
        auto revealed = st.response_stream.revealed_text();
        Children response_children;
        if (!revealed.empty()) {
            response_children.push_back(Markdown({.source = revealed}));
        }
        rows.push_back(
            MessageBubble({
                .role = Role::Assistant,
                .children = std::move(response_children),
                .is_streaming = still_streaming,
                .frame = st.frame,
            })
        );

        if (st.response_stream.fully_revealed() || phase == Phase::Complete) {
            rows.push_back(InlineDiff({
                .before = "const SESSION_TIMEOUT = 3600; // seconds",
                .after  = "const TOKEN_EXPIRY = '1h';",
                .label  = "Key fix",
            }));
        }
    }

    // ── Completion section ─────────────────────────────────────
    if (phase == Phase::Complete) {
        rows.push_back(
            Callout({
                .severity = Severity::Success,
                .title = "Task completed  +65 -22",
                .body = "Refactored session auth to JWT. All tests passing.",
            })
        );

        rows.push_back(hstack().gap(2)(
            Chip({.label = "14/14 tests", .severity = Severity::Success}),
            Chip({.label = "3 files"}),
            Chip({.label = "2 deps swapped", .severity = Severity::Info}),
            DiffStat({.added = 65, .removed = 22})
        ));

        rows.push_back(st.checkpoint.render());
        rows.push_back(Divider({.color = p.dim}));

        rows.push_back(FormField({
            .label = "Cost & Context",
            .description = "Session resource usage",
            .children = {
                CostMeter({
                    .input_tokens = st.total_input_tokens,
                    .output_tokens = st.total_output_tokens,
                    .cache_read = st.cache_read,
                    .cache_write = st.cache_write,
                    .cost = st.total_cost,
                    .budget = 5.00,
                    .model = "claude-opus-4-6",
                }),
            },
        }));

        rows.push_back(ContextWindow({
            .segments = {
                {"System",   12400, p.info},
                {"History",  static_cast<int>(st.total_input_tokens * 0.6), p.secondary},
                {"Tools",    static_cast<int>(st.total_input_tokens * 0.3), p.accent},
                {"Response", st.total_output_tokens, p.success},
                {"Cache",    st.cache_read, Color::rgb(100, 180, 100)},
            },
            .max_tokens = 200000,
            .width = 40,
        }));

        {
            int total_used = 12400 + st.total_input_tokens + st.total_output_tokens + st.cache_read;
            float ctx_frac = static_cast<float>(total_used) / 200000.f;
            rows.push_back(Gauge({
                .value = ctx_frac,
                .label = "Context ",
                .width = 40,
                .thresholds = {{0.5f, p.success}, {0.75f, p.warning}, {0.9f, p.error}},
            }));
        }

        {
            float cur_rate = st.tok_rate_history.empty() ? 0.f : st.tok_rate_history.back();
            rows.push_back(TokenStream({
                .total_tokens = st.total_output_tokens,
                .tokens_per_sec = cur_rate,
                .peak_rate = st.peak_tok_rate,
                .elapsed_secs = st.elapsed,
                .rate_history = st.tok_rate_history,
            }));
        }

        rows.push_back(hstack().gap(1)(
            muted("tok/s"),
            Sparkline({.data = st.tok_rate_history, .width = 30,
                       .color = Color::rgb(255, 200, 100)})
        ));

        if (!st.heatmap_data.empty() && !st.heatmap_data[0].empty()) {
            rows.push_back(FormField({
                .label = "Token Activity",
                .children = {
                    Heatmap({
                        .data = st.heatmap_data,
                        .row_labels = {"in", "out", "cache", "rate"},
                    }),
                },
            }));
        }

        rows.push_back(st.stats_disclosure.render({
            hstack().gap(2)(
                muted(fmt("Input: %.1fk tokens", st.total_input_tokens / 1000.0)),
                muted(fmt("Output: %.1fk tokens", st.total_output_tokens / 1000.0))
            ),
            hstack().gap(2)(
                muted(fmt("Cache read: %d", st.cache_read)),
                muted(fmt("Cache write: %d", st.cache_write))
            ),
            hstack().gap(2)(
                muted(fmt("Peak rate: %.0f tok/s", static_cast<double>(st.peak_tok_rate))),
                muted(fmt("Elapsed: %.1fs", static_cast<double>(st.elapsed)))
            ),
        }));

        rows.push_back(Divider({.color = p.dim}));
        rows.push_back(st.feedback.render());

        if (!st.toasts.empty()) {
            rows.push_back(st.toasts.render());
        }
    }

    // ── Keybindings (always visible) ─────────────────────────
    rows.push_back(KeyBindings({
        {.keys = "Esc", .label = "quit"},
        {.keys = "1-5", .label = "toggle tools"},
    }));

    // ── Status bar (always visible) ──────────────────────────
    {
        const char* phase_label = "Ready";
        Color phase_color = p.success;
        switch (phase) {
            case Phase::UserMessage:  phase_label = "Receiving"; phase_color = p.info; break;
            case Phase::Thinking:     phase_label = "Thinking";  phase_color = p.warning; break;
            case Phase::ReadFile:     phase_label = "Reading";   phase_color = p.info; break;
            case Phase::EditAuth:
            case Phase::CreateToken:
            case Phase::EditTests:    phase_label = "Editing";   phase_color = p.accent; break;
            case Phase::RunTests:     phase_label = "Testing";   phase_color = p.warning; break;
            case Phase::Responding:   phase_label = "Writing";   phase_color = p.primary; break;
            case Phase::Complete:     phase_label = "Done";      phase_color = p.success; break;
        }

        int plan_done = 0;
        if (phase > Phase::ReadFile) plan_done++;
        if (phase > Phase::EditAuth) plan_done++;
        if (phase > Phase::CreateToken) plan_done++;
        if (phase > Phase::EditTests) plan_done++;
        if (phase > Phase::RunTests) plan_done++;

        bool active = phase != Phase::Complete;
        int secs = static_cast<int>(st.elapsed);

        std::string status_str = active
            ? std::string(spin(st.frame)) + " " + phase_label
            : std::string("✓ ") + phase_label;

        rows.push_back(StatusBar(StatusBarProps{.sections = {
            {.content = " maya", .color = p.primary, .bold = true},
            {.content = "claude-opus-4-6", .color = p.accent},
            {.content = status_str, .color = phase_color, .bold = true},
            {.content = fmt("%d/5", plan_done), .color = plan_done == 5 ? p.success : p.muted},
            {.content = fmt("$%.3f", st.total_cost),
             .color = st.total_cost > 0.5 ? p.warning : p.muted},
            {.content = fmt("%ds", secs), .color = p.dim},
        }}));
    }

    return vstack().gap(1)(std::move(rows));
}

// ═══════════════════════════════════════════════════════════════════════════════
// Main
// ═══════════════════════════════════════════════════════════════════════════════

int main() {
    State st;
    using Clock = std::chrono::steady_clock;
    auto last = Clock::now();

    run(
        {.fps = 30, .alt_screen = false},

        // ── Event handler ──────────────────────────────────────
        [&](const Event& ev) {
            if (key(ev, 'q') || key(ev, SpecialKey::Escape)) return false;

            // Toggle tool collapse with number keys
            if (key(ev, '1')) { st.tool1_collapsed = !st.tool1_collapsed; st.read_card.toggle_collapsed(); return true; }
            if (key(ev, '2')) { st.tool2_collapsed = !st.tool2_collapsed; return true; }
            if (key(ev, '3')) { st.tool3_collapsed = !st.tool3_collapsed; return true; }
            if (key(ev, '4')) { st.tool4_collapsed = !st.tool4_collapsed; return true; }
            if (key(ev, '5')) { st.tool5_collapsed = !st.tool5_collapsed; return true; }

            // Toggle detailed stats disclosure
            if (key(ev, 'd')) { st.stats_disclosure.toggle(); return true; }

            // Feedback
            st.feedback.update(ev);

            // Checkpoint
            st.checkpoint.update(ev);

            return true;
        },

        // ── Render ─────────────────────────────────────────────
        [&] {
            auto now = Clock::now();
            float dt = std::chrono::duration<float>(now - last).count();
            last = now;
            advance(st, dt);
            return build_ui(st);
        }
    );
}
