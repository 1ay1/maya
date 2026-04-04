// maya — Full coding agent session simulator
//
// A richly detailed simulation of an AI coding agent working through a complex
// multi-file refactoring task. Demonstrates every maya component in a realistic
// agentic workflow: streaming chat, tool calls with diffs, file navigation,
// live test output, plan tracking, and configuration.
//
// Usage: ./maya_agent_session
//        Tab/BackTab to switch panels, j/k/arrows to navigate, q to quit.

#include <maya/maya.hpp>
#include <maya/dsl.hpp>
#include <maya/components/components.hpp>

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

    // ── Tab layout ─────────────────────────────────────────────
    Tabs main_tabs{TabsProps{.labels = {
        " Chat ", " Files ", " Tests ", " Config "
    }}};

    // ── Chat tab state ─────────────────────────────────────────
    ThinkingBlock thinking{ThinkingBlockProps{
        .content = "",
        .expanded = true,
        .is_streaming = true,
    }};
    StreamingText thinking_stream{StreamingTextProps{
        .text = kThinkingContent, .show_cursor = false,
    }};
    int thinking_chars = 0;

    StreamingText response_stream{StreamingTextProps{
        .text = kAssistantResponse, .show_cursor = false,
    }};
    bool response_started = false;

    // Tool call states
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

    ScrollView chat_scroll{ScrollViewProps{
        .max_visible = 40,
        .tail_follow = true,
    }};

    // ── Files tab state ────────────────────────────────────────
    FileTree file_tree{FileTreeProps{
        .roots = FileTree::from_paths({
            "src/middleware/auth.ts",
            "src/middleware/cors.ts",
            "src/middleware/rate-limit.ts",
            "src/utils/token.ts",
            "src/utils/hash.ts",
            "src/utils/logger.ts",
            "src/routes/login.ts",
            "src/routes/users.ts",
            "src/routes/projects.ts",
            "src/types/auth.ts",
            "src/types/user.ts",
            "src/config/database.ts",
            "src/config/env.ts",
            "src/app.ts",
            "src/index.ts",
            "tests/auth.test.ts",
            "tests/token.test.ts",
            "tests/routes/login.test.ts",
            "tests/helpers/mock.ts",
            "package.json",
            "tsconfig.json",
            ".env.example",
        }),
        .show_icons = true,
        .show_git_status = true,
        .max_visible = 18,
    }};

    Table deps_table{TableProps{
        .columns = {
            {.header = "Package",     .width = 24},
            {.header = "Version",     .width = 10, .align = ColumnAlign::Right},
            {.header = "Status",      .width = 12, .align = ColumnAlign::Center},
            {.header = "Size",        .width = 10, .align = ColumnAlign::Right},
        },
        .rows = {
            {"jsonwebtoken",     "9.0.2",   "Added",     "45KB"},
            {"@types/jsonwebtoken", "9.0.5", "Added",    "12KB"},
            {"express-session",  "1.18.0",  "Removed",   "52KB"},
            {"connect-redis",    "7.1.0",   "Removed",   "18KB"},
            {"express",          "4.19.2",  "Unchanged", "210KB"},
            {"typescript",       "5.4.2",   "Unchanged", "38MB"},
            {"vitest",           "1.4.0",   "Unchanged", "8.2MB"},
            {"cors",             "2.8.5",   "Unchanged", "6KB"},
        },
        .max_visible = 8,
    }};

    // ── Tests tab state ────────────────────────────────────────
    LogView test_log{LogViewProps{
        .max_visible = 16,
        .tail_follow = true,
        .show_line_nums = true,
    }};
    int test_line = 0;
    float test_timer = 0.f;

    LogView server_log{LogViewProps{
        .max_visible = 8,
        .tail_follow = true,
    }};
    int server_line = 0;
    float server_timer = 0.f;

    // Metrics
    std::vector<float> cpu_history;
    std::vector<float> mem_history;
    std::vector<float> latency_history;

    // ── Config tab state ───────────────────────────────────────
    RadioGroup<std::string> model_select{RadioGroupProps<std::string>{
        .items = {"claude-opus-4-6", "claude-sonnet-4-6", "claude-haiku-4-5"},
        .selected = 0,
    }};

    NumberInput max_tokens{NumberInputProps{
        .value = 8192, .min = 256, .max = 128000, .step = 256,
        .label = "Max tokens",
    }};

    NumberInput temperature{NumberInputProps{
        .value = 0, .min = 0, .max = 100, .step = 5,
        .label = "Temperature",
    }};

    NumberInput budget_input{NumberInputProps{
        .value = 500, .min = 0, .max = 10000, .step = 50,
        .label = "Budget (cents)",
    }};

    bool auto_approve = false;
    bool diff_preview = true;
    bool run_tests    = true;
    bool streaming    = true;
    bool dark_mode    = true;
    bool telemetry    = false;
    bool verbose_logs = false;

    // ── Plan tracking ──────────────────────────────────────────
    ActivityBar activity{ActivityBarProps{
        .plan_items = {
            {.text = "Read current auth middleware",           .status = TaskStatus::Pending},
            {.text = "Replace session auth with JWT",         .status = TaskStatus::Pending},
            {.text = "Create token utility module",           .status = TaskStatus::Pending},
            {.text = "Update test suite",                     .status = TaskStatus::Pending},
            {.text = "Run tests and verify",                  .status = TaskStatus::Pending},
        },
        .edits = {},
    }};

    // ── Cost tracking ──────────────────────────────────────────
    int total_input_tokens = 0;
    int total_output_tokens = 0;
    int cache_read = 0;
    int cache_write = 0;
    double total_cost = 0.0;

    // ── Token rate tracking (for TokenStream) ────────────────
    std::vector<float> tok_rate_history;
    float tok_rate_timer = 0.f;
    float peak_tok_rate = 0.f;
    int   prev_output_tokens = 0;
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
            st.activity.set_plan({
                {.text = "Read current auth middleware",   .status = TaskStatus::InProgress},
                {.text = "Replace session auth with JWT", .status = TaskStatus::Pending},
                {.text = "Create token utility module",   .status = TaskStatus::Pending},
                {.text = "Update test suite",             .status = TaskStatus::Pending},
                {.text = "Run tests and verify",          .status = TaskStatus::Pending},
            });
        }
        if (st.phase_timer > 1.2f) {
            st.read_status = TaskStatus::Completed;
            st.tool1_collapsed = true;
            st.total_input_tokens += 3200;
            st.cache_write += 1200;
            st.total_cost += 0.048;
            st.activity.set_plan({
                {.text = "Read current auth middleware",   .status = TaskStatus::Completed},
                {.text = "Replace session auth with JWT", .status = TaskStatus::Pending},
                {.text = "Create token utility module",   .status = TaskStatus::Pending},
                {.text = "Update test suite",             .status = TaskStatus::Pending},
                {.text = "Run tests and verify",          .status = TaskStatus::Pending},
            });
            enter_phase(st, Phase::EditAuth);
        }
        break;

    case Phase::EditAuth:
        if (just) {
            st.edit1_status = TaskStatus::InProgress;
            st.activity.set_plan({
                {.text = "Read current auth middleware",   .status = TaskStatus::Completed},
                {.text = "Replace session auth with JWT", .status = TaskStatus::InProgress},
                {.text = "Create token utility module",   .status = TaskStatus::Pending},
                {.text = "Update test suite",             .status = TaskStatus::Pending},
                {.text = "Run tests and verify",          .status = TaskStatus::Pending},
            });
        }
        if (st.phase_timer > 2.0f) {
            st.edit1_status = TaskStatus::Completed;
            st.tool2_collapsed = true;
            st.total_output_tokens += 1840;
            st.total_cost += 0.138;
            st.activity.set_plan({
                {.text = "Read current auth middleware",   .status = TaskStatus::Completed},
                {.text = "Replace session auth with JWT", .status = TaskStatus::Completed},
                {.text = "Create token utility module",   .status = TaskStatus::Pending},
                {.text = "Update test suite",             .status = TaskStatus::Pending},
                {.text = "Run tests and verify",          .status = TaskStatus::Pending},
            });
            st.activity.set_edits({
                {.path = "src/middleware/auth.ts", .added = 18, .removed = 14},
            });
            enter_phase(st, Phase::CreateToken);
        }
        break;

    case Phase::CreateToken:
        if (just) {
            st.create_status = TaskStatus::InProgress;
            st.activity.set_plan({
                {.text = "Read current auth middleware",   .status = TaskStatus::Completed},
                {.text = "Replace session auth with JWT", .status = TaskStatus::Completed},
                {.text = "Create token utility module",   .status = TaskStatus::InProgress},
                {.text = "Update test suite",             .status = TaskStatus::Pending},
                {.text = "Run tests and verify",          .status = TaskStatus::Pending},
            });
        }
        if (st.phase_timer > 2.0f) {
            st.create_status = TaskStatus::Completed;
            st.tool3_collapsed = true;
            st.total_output_tokens += 2100;
            st.total_cost += 0.158;
            st.activity.set_plan({
                {.text = "Read current auth middleware",   .status = TaskStatus::Completed},
                {.text = "Replace session auth with JWT", .status = TaskStatus::Completed},
                {.text = "Create token utility module",   .status = TaskStatus::Completed},
                {.text = "Update test suite",             .status = TaskStatus::Pending},
                {.text = "Run tests and verify",          .status = TaskStatus::Pending},
            });
            st.activity.set_edits({
                {.path = "src/middleware/auth.ts", .added = 18, .removed = 14},
                {.path = "src/utils/token.ts",    .added = 32, .removed = 0},
            });
            enter_phase(st, Phase::EditTests);
        }
        break;

    case Phase::EditTests:
        if (just) {
            st.edit2_status = TaskStatus::InProgress;
            st.activity.set_plan({
                {.text = "Read current auth middleware",   .status = TaskStatus::Completed},
                {.text = "Replace session auth with JWT", .status = TaskStatus::Completed},
                {.text = "Create token utility module",   .status = TaskStatus::Completed},
                {.text = "Update test suite",             .status = TaskStatus::InProgress},
                {.text = "Run tests and verify",          .status = TaskStatus::Pending},
            });
        }
        if (st.phase_timer > 2.0f) {
            st.edit2_status = TaskStatus::Completed;
            st.tool4_collapsed = true;
            st.total_output_tokens += 1650;
            st.total_cost += 0.124;
            st.activity.set_plan({
                {.text = "Read current auth middleware",   .status = TaskStatus::Completed},
                {.text = "Replace session auth with JWT", .status = TaskStatus::Completed},
                {.text = "Create token utility module",   .status = TaskStatus::Completed},
                {.text = "Update test suite",             .status = TaskStatus::Completed},
                {.text = "Run tests and verify",          .status = TaskStatus::Pending},
            });
            st.activity.set_edits({
                {.path = "src/middleware/auth.ts", .added = 18, .removed = 14},
                {.path = "src/utils/token.ts",    .added = 32, .removed = 0},
                {.path = "tests/auth.test.ts",    .added = 15, .removed = 8},
            });
            enter_phase(st, Phase::RunTests);
        }
        break;

    case Phase::RunTests:
        if (just) {
            st.test_status = TaskStatus::InProgress;
            st.test_line = 0;
            st.test_timer = 0.f;
            st.activity.set_plan({
                {.text = "Read current auth middleware",   .status = TaskStatus::Completed},
                {.text = "Replace session auth with JWT", .status = TaskStatus::Completed},
                {.text = "Create token utility module",   .status = TaskStatus::Completed},
                {.text = "Update test suite",             .status = TaskStatus::Completed},
                {.text = "Run tests and verify",          .status = TaskStatus::InProgress},
            });
        }
        // Stream test output
        st.test_timer += dt;
        if (st.test_timer > 0.12f && st.test_line < kNumTestLines) {
            st.test_timer = 0.f;
            st.test_log.append(kTestLines[st.test_line]);
            st.test_line++;
        }
        if (st.test_line >= kNumTestLines && st.phase_timer > 4.f) {
            st.test_status = TaskStatus::Completed;
            st.tool5_collapsed = true;
            st.total_input_tokens += 1200;
            st.total_cost += 0.018;
            st.activity.set_plan({
                {.text = "Read current auth middleware",   .status = TaskStatus::Completed},
                {.text = "Replace session auth with JWT", .status = TaskStatus::Completed},
                {.text = "Create token utility module",   .status = TaskStatus::Completed},
                {.text = "Update test suite",             .status = TaskStatus::Completed},
                {.text = "Run tests and verify",          .status = TaskStatus::Completed},
            });
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
        break;
    }
}

static void advance_metrics(State& st, float dt) {
    float t = st.elapsed;
    float cpu = 0.15f + 0.1f * std::sin(t * 0.8f) + 0.05f * std::sin(t * 2.3f);
    float mem = 0.42f + 0.08f * std::sin(t * 0.2f) + 0.03f * std::sin(t * 1.1f);
    float lat = 0.1f + 0.3f * std::sin(t * 0.5f) * std::sin(t * 0.5f);

    // Spike during test runs
    if (st.phase == Phase::RunTests) {
        cpu += 0.35f;
        mem += 0.12f;
        lat += 0.15f;
    }

    st.cpu_history.push_back(std::clamp(cpu, 0.f, 1.f));
    st.mem_history.push_back(std::clamp(mem, 0.f, 1.f));
    st.latency_history.push_back(std::clamp(lat, 0.f, 1.f));
    auto trim = [](std::vector<float>& v, size_t max) {
        if (v.size() > max) v.erase(v.begin());
    };
    trim(st.cpu_history, 50);
    trim(st.mem_history, 50);
    trim(st.latency_history, 50);

    // Token rate tracking
    st.tok_rate_timer += dt;
    if (st.tok_rate_timer > 0.3f) {
        st.tok_rate_timer = 0.f;
        int delta = st.total_output_tokens - st.prev_output_tokens;
        float rate = static_cast<float>(delta) / 0.3f;
        st.prev_output_tokens = st.total_output_tokens;
        st.tok_rate_history.push_back(rate);
        if (rate > st.peak_tok_rate) st.peak_tok_rate = rate;
        trim(st.tok_rate_history, 40);
    }

    // Server logs
    st.server_timer += dt;
    if (st.server_timer > 1.2f) {
        st.server_timer = 0.f;
        st.server_log.append(kServerLogs[st.server_line % kNumServerLogs]);
        st.server_line++;
    }
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

static constexpr auto kLeftOnly = BorderSides{
    .top = false, .right = false, .bottom = false, .left = true};

// Accent left-border section with padding
static Element section(Color accent, Children children) {
    return vstack()
        .border_sides(kLeftOnly)
        .border_color(accent)
        .padding(0, 1, 0, 1)
        .gap(0)(std::move(children));
}

// Titled panel with round border
static Element panel(const std::string& title, Color border_col, Children children) {
    return vstack()
        .border(Round, border_col)
        .border_text(title, BorderTextPos::Top, BorderTextAlign::Start)
        .padding(0, 1, 0, 1)
        .gap(1)(std::move(children));
}

// Muted dim label
static Element label(const std::string& s) {
    return text(s, Style{}.with_fg(palette().muted).with_dim());
}

// ═══════════════════════════════════════════════════════════════════════════════
// Render: Chat tab
// ═══════════════════════════════════════════════════════════════════════════════

static Element build_chat(State& st) {
    auto& p = palette();
    std::vector<Element> rows;

    // ── User message ───────────────────────────────────────────
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

    if (st.phase < Phase::Thinking)
        return vstack().padding(0, 1, 0, 1).gap(1)(std::move(rows));

    // ── Thinking block ─────────────────────────────────────────
    rows.push_back(
        MessageBubble({
            .role = Role::Assistant,
            .children = {st.thinking.render(st.frame)},
            .is_streaming = st.phase == Phase::Thinking,
            .frame = st.frame,
        })
    );

    if (st.phase < Phase::ReadFile)
        return vstack().padding(0, 1, 0, 1).gap(1)(std::move(rows));

    // ── Tool 1: Read file ──────────────────────────────────────
    rows.push_back(ToolCard({
        .title = "Read src/middleware/auth.ts",
        .status = st.read_status,
        .frame = st.frame,
        .collapsed = st.tool1_collapsed,
        .summary = st.read_status == TaskStatus::Completed ? "42 lines read" : "",
        .children = st.tool1_collapsed ? Children{} : Children{
            CodeBlock({
                .code = "import session from 'express-session';\n"
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
                .language = "typescript",
            }),
        },
        .tool_name = "Read",
    }));

    if (st.phase < Phase::EditAuth)
        return vstack().padding(0, 1, 0, 1).gap(1)(std::move(rows));

    // ── Tool 2: Edit auth middleware ────────────────────────────
    rows.push_back(ToolCard({
        .title = "Edit src/middleware/auth.ts",
        .status = st.edit1_status,
        .frame = st.frame,
        .collapsed = st.tool2_collapsed,
        .summary = st.edit1_status == TaskStatus::Completed ? "+18 -14 lines" : "",
        .children = st.tool2_collapsed ? Children{} : Children{
            DiffView({.diff = kDiff1, .file_path = "src/middleware/auth.ts"}),
        },
        .tool_name = "Edit",
    }));

    if (st.phase < Phase::CreateToken)
        return vstack().padding(0, 1, 0, 1).gap(1)(std::move(rows));

    // ── Tool 3: Create token utility ───────────────────────────
    rows.push_back(ToolCard({
        .title = "Write src/utils/token.ts",
        .status = st.create_status,
        .frame = st.frame,
        .collapsed = st.tool3_collapsed,
        .summary = st.create_status == TaskStatus::Completed ? "+32 lines (new file)" : "",
        .children = st.tool3_collapsed ? Children{} : Children{
            DiffView({.diff = kDiff2, .file_path = "src/utils/token.ts"}),
        },
        .tool_name = "Write",
    }));

    if (st.phase < Phase::EditTests)
        return vstack().padding(0, 1, 0, 1).gap(1)(std::move(rows));

    // ── Tool 4: Edit tests ─────────────────────────────────────
    rows.push_back(ToolCard({
        .title = "Edit tests/auth.test.ts",
        .status = st.edit2_status,
        .frame = st.frame,
        .collapsed = st.tool4_collapsed,
        .summary = st.edit2_status == TaskStatus::Completed ? "+15 -8 lines" : "",
        .children = st.tool4_collapsed ? Children{} : Children{
            DiffView({.diff = kDiff3, .file_path = "tests/auth.test.ts"}),
        },
        .tool_name = "Edit",
    }));

    if (st.phase < Phase::RunTests)
        return vstack().padding(0, 1, 0, 1).gap(1)(std::move(rows));

    // ── Tool 5: Run tests ──────────────────────────────────────
    {
        Children test_children;
        if (!st.tool5_collapsed) {
            test_children.push_back(st.test_log.render());
        }
        std::string summary;
        if (st.test_status == TaskStatus::Completed)
            summary = "14 passed, 0 failed";
        else if (st.test_line > 0)
            summary = std::to_string(st.test_line) + "/" + std::to_string(kNumTestLines) + " lines";

        rows.push_back(ToolCard({
            .title = "Run npx vitest run --filter auth",
            .status = st.test_status,
            .frame = st.frame,
            .collapsed = st.tool5_collapsed,
            .summary = summary,
            .children = std::move(test_children),
            .tool_name = "Bash",
        }));
    }

    if (st.phase < Phase::Responding)
        return vstack().padding(0, 1, 0, 1).gap(1)(std::move(rows));

    // ── Assistant response (rendered as Markdown) ────────────
    {
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
    }

    if (st.phase == Phase::Complete) {
        rows.push_back(
            vstack()
                .border(Round, p.success)
                .padding(0, 1, 0, 1)
                .gap(1)(
                hstack().gap(2)(
                    text("✓", Style{}.with_fg(p.success).with_bold()),
                    text("Task completed", Style{}.with_fg(p.success).with_bold()),
                    DiffStat({.added = 65, .removed = 22})
                ),
                hstack().gap(2)(
                    Chip({.label = "14/14 tests pass", .severity = Severity::Success}),
                    Chip({.label = "3 files changed"}),
                    Chip({.label = "2 deps swapped", .severity = Severity::Info}),
                    text(fmt("$%.4f", st.total_cost), Style{}.with_fg(p.muted))
                )
            )
        );
    }

    return vstack().padding(0, 1, 0, 1).gap(1)(std::move(rows));
}

// ═══════════════════════════════════════════════════════════════════════════════
// Render: Sidebar — plan + edits + cost + metrics
// ═══════════════════════════════════════════════════════════════════════════════

static Element sidebar_heading(const std::string& title, const Palette& p) {
    return text(title, Style{}.with_fg(p.muted).with_bold().with_dim());
}

static Element build_sidebar(State& st) {
    auto& p = palette();
    bool is_done = st.phase == Phase::Complete;

    std::vector<Element> rows;

    // ── Plan ────────────────────────────────────────────────────
    rows.push_back(sidebar_heading("PLAN", p));

    struct PlanStep { const char* label; Phase after; };
    PlanStep steps[] = {
        {"Read auth middleware",     Phase::ReadFile},
        {"Replace session with JWT", Phase::EditAuth},
        {"Create token utility",     Phase::CreateToken},
        {"Update test suite",        Phase::EditTests},
        {"Run tests & verify",       Phase::RunTests},
    };

    int done_count = 0;
    for (auto& step : steps) {
        bool done = st.phase > step.after;
        bool active = false;
        if (step.after == Phase::ReadFile    && st.phase == Phase::ReadFile)    active = true;
        if (step.after == Phase::EditAuth    && st.phase == Phase::EditAuth)    active = true;
        if (step.after == Phase::CreateToken && st.phase == Phase::CreateToken) active = true;
        if (step.after == Phase::EditTests   && st.phase == Phase::EditTests)   active = true;
        if (step.after == Phase::RunTests    && st.phase == Phase::RunTests)    active = true;

        if (done) done_count++;

        Element icon_elem = active
            ? Spinner({.frame = st.frame, .style = SpinnerStyle::Dots, .color = p.primary})
            : text(done ? "✓" : "·",
                   Style{}.with_fg(done ? p.success : p.dim).with_bold());

        Style label_style = done
            ? Style{}.with_fg(p.dim).with_strikethrough()
            : active
                ? Style{}.with_fg(p.text).with_bold()
                : Style{}.with_fg(p.muted);

        rows.push_back(hstack().gap(1)(
            std::move(icon_elem),
            text(step.label, label_style)
        ));
    }

    float plan_frac = static_cast<float>(done_count) / 5.f;
    rows.push_back(hstack().gap(1)(
        ProgressBar({
            .value = plan_frac,
            .width = 24,
            .show_percent = false,
            .filled = is_done ? p.success : p.primary,
        }),
        text(fmt("%d/5", done_count), Style{}.with_fg(p.muted))
    ));

    // ── Divider ─────────────────────────────────────────────────
    rows.push_back(Divider({.color = p.dim}));

    // ── Edits ───────────────────────────────────────────────────
    rows.push_back(sidebar_heading("EDITS", p));

    struct EditInfo { const char* path; int add; int del; Phase after; };
    EditInfo edits[] = {
        {"middleware/auth.ts", 18, 14, Phase::EditAuth},
        {"utils/token.ts",    32, 0,  Phase::CreateToken},
        {"auth.test.ts",      15, 8,  Phase::EditTests},
    };
    bool has_edits = false;
    for (auto& e : edits) {
        if (st.phase > e.after) {
            has_edits = true;
            rows.push_back(hstack().gap(1)(
                text(e.path, Style{}.with_fg(p.text)),
                DiffStat({.added = e.add, .removed = e.del})
            ));
        }
    }
    if (!has_edits) {
        rows.push_back(
            text("waiting...", Style{}.with_fg(p.dim).with_italic()));
    }

    rows.push_back(Divider({.color = p.dim}));

    // ── Context ─────────────────────────────────────────────────
    rows.push_back(sidebar_heading("CONTEXT", p));
    rows.push_back(ContextWindow({
        .segments = {
            {"System",   12400, p.info},
            {"History",  static_cast<int>(st.total_input_tokens * 0.6), p.secondary},
            {"Tools",    static_cast<int>(st.total_input_tokens * 0.3), p.accent},
            {"Response", st.total_output_tokens, p.success},
        },
        .max_tokens = 200000,
        .width = 26,
        .show_labels = false,
    }));

    rows.push_back(Divider({.color = p.dim}));

    // ── Stats (cost + time + rate in a compact block) ───────────
    rows.push_back(sidebar_heading("STATS", p));

    int secs = static_cast<int>(st.elapsed);
    int mins = secs / 60;
    secs %= 60;

    float cur_rate = st.tok_rate_history.empty() ? 0.f : st.tok_rate_history.back();
    Color rate_col = cur_rate > 50.f ? Color::rgb(80, 220, 120)
                   : cur_rate > 20.f ? Color::rgb(240, 200, 60)
                   : Color::rgb(240, 80, 80);

    auto stat_row = [&](const std::string& lbl, Element val) {
        return hstack().gap(1)(
            text(fmt("%-8s", lbl.c_str()), Style{}.with_fg(p.dim)),
            std::move(val)
        );
    };

    rows.push_back(stat_row("Cost",
        text(fmt("$%.4f / $5.00", st.total_cost),
             Style{}.with_fg(st.total_cost > 4.0 ? p.error
                           : st.total_cost > 2.5 ? p.warning
                           : p.success))));
    rows.push_back(stat_row("Elapsed",
        text(fmt("%d:%02d", mins, secs), Style{}.with_fg(p.text))));
    rows.push_back(stat_row("Rate",
        text(fmt("%.0f tok/s", static_cast<double>(cur_rate)),
             Style{}.with_fg(rate_col))));
    rows.push_back(stat_row("Input",
        text(fmt("%.1fk", st.total_input_tokens / 1000.0),
             Style{}.with_fg(p.muted))));
    rows.push_back(stat_row("Output",
        text(fmt("%.1fk", st.total_output_tokens / 1000.0),
             Style{}.with_fg(p.muted))));

    rows.push_back(Divider({.color = p.dim}));

    // ── Sparklines ──────────────────────────────────────────────
    rows.push_back(hstack().gap(1)(
        text("cpu", Style{}.with_fg(p.dim)),
        Sparkline({.data = st.cpu_history, .width = 22, .color = Color::rgb(100, 200, 255)})
    ));
    rows.push_back(hstack().gap(1)(
        text("mem", Style{}.with_fg(p.dim)),
        Sparkline({.data = st.mem_history, .width = 22, .color = Color::rgb(200, 120, 255)})
    ));
    rows.push_back(hstack().gap(1)(
        text("tok", Style{}.with_fg(p.dim)),
        Sparkline({.data = st.tok_rate_history, .width = 22, .color = Color::rgb(255, 200, 100)})
    ));

    // ── Single bordered container ───────────────────────────────
    return vstack()
        .border(Round, p.border)
        .padding(0, 1, 0, 1)
        .gap(0)(std::move(rows));
}

// ═══════════════════════════════════════════════════════════════════════════════
// Render: Files tab
// ═══════════════════════════════════════════════════════════════════════════════

static Element build_files(State& st) {
    auto& p = palette();

    // File tree
    auto tree_panel = panel(" Project ", p.primary, {
        st.file_tree.render(),
        hstack().gap(2)(
            Chip({.label = "22 files"}),
            Chip({.label = "3 modified", .severity = Severity::Warning}),
            Chip({.label = "1 new", .severity = Severity::Success})
        ),
    });

    // Code preview
    auto preview_panel = panel(" src/middleware/auth.ts ", p.accent, {
        CodeBlock({
            .code = kCodePreview,
            .language = "typescript",
            .highlight_lines = {8, 11, 14, 18},
        }),
    });

    // Dependencies
    auto deps_panel = panel(" Dependencies ", p.border, {
        st.deps_table.render(),
        hstack().gap(2)(
            Chip({.label = "+2 added", .severity = Severity::Info}),
            Chip({.label = "-2 removed", .severity = Severity::Error}),
            text("net: -13KB", Style{}.with_fg(p.success))
        ),
    });

    return vstack().padding(0, 1, 0, 1).gap(1)(
        std::move(tree_panel),
        std::move(preview_panel),
        std::move(deps_panel)
    );
}

// ═══════════════════════════════════════════════════════════════════════════════
// Render: Tests tab
// ═══════════════════════════════════════════════════════════════════════════════

static Element build_tests(State& st) {
    auto& p = palette();

    float test_progress = 0.f;
    if (st.phase > Phase::RunTests) test_progress = 1.f;
    else if (st.phase == Phase::RunTests)
        test_progress = static_cast<float>(st.test_line) / kNumTestLines;

    bool tests_done = test_progress >= 1.f;

    // Title with live spinner
    std::string test_title = tests_done
        ? " ✓ Test Output "
        : (st.phase == Phase::RunTests
            ? std::string(" ") + spin(st.frame) + " Test Output "
            : " Test Output ");

    // Test output panel
    auto test_panel = panel(test_title, tests_done ? p.success : p.border, {
        st.test_log.render(),
        hstack().gap(2)(
            label("progress"),
            ProgressBar({
                .value = test_progress,
                .width = 30,
                .show_percent = true,
                .filled = tests_done ? p.success : p.primary,
            })
        ),
    });

    // Results panel — only show when tests are done
    Children results_children;
    if (tests_done) {
        results_children.push_back(BarChart({.bars = {
            {.label = "auth.test",  .value = 5, .color = p.success},
            {.label = "token.test", .value = 5, .color = p.success},
            {.label = "login.test", .value = 4, .color = p.success},
        }, .max_width = 20}));
        results_children.push_back(
            hstack().gap(2)(
                Chip({.label = "3 suites", .severity = Severity::Success}),
                Chip({.label = "14 tests", .severity = Severity::Success}),
                Chip({.label = "1.847s"})
            )
        );
    } else {
        results_children.push_back(
            text("waiting for test run...", Style{}.with_fg(p.dim).with_italic()));
    }
    auto results_panel = panel(" Results ", p.border, std::move(results_children));

    // Server panel
    auto server_panel = panel(" Dev Server ", p.border, {
        st.server_log.render(),
    });

    // Metrics panel — sparklines + gauges
    float cpu_val = st.cpu_history.empty() ? 0.f : st.cpu_history.back();
    float mem_val = st.mem_history.empty() ? 0.f : st.mem_history.back();
    float lat_val = st.latency_history.empty() ? 0.f : st.latency_history.back();

    auto metrics_panel = panel(" System Metrics ", p.border, {
        hstack().gap(1)(
            label("CPU"),
            Sparkline({.data = st.cpu_history, .width = 30, .color = Color::rgb(100, 200, 255)}),
            text(fmt("%3.0f%%", static_cast<double>(cpu_val * 100)),
                 Style{}.with_fg(cpu_val > 0.6f ? p.warning : p.text).with_bold())
        ),
        hstack().gap(1)(
            label("MEM"),
            Sparkline({.data = st.mem_history, .width = 30, .color = Color::rgb(200, 120, 255)}),
            text(fmt("%3.0f%%", static_cast<double>(mem_val * 100)),
                 Style{}.with_fg(mem_val > 0.7f ? p.warning : p.text).with_bold())
        ),
        hstack().gap(1)(
            label("LAT"),
            Sparkline({.data = st.latency_history, .width = 30, .color = Color::rgb(255, 180, 100)}),
            text(fmt("%4.0fms", static_cast<double>(lat_val * 200)),
                 Style{}.with_fg(lat_val > 0.5f ? p.warning : p.text).with_bold())
        ),
        Gauge({.value = cpu_val, .label = "CPU Load ", .width = 30,
               .thresholds = {{0.3f, p.success}, {0.6f, p.warning}, {0.85f, p.error}}}),
        Gauge({.value = mem_val, .label = "Memory   ", .width = 30,
               .thresholds = {{0.5f, p.success}, {0.7f, p.warning}, {0.9f, p.error}}}),
        Gauge({.value = 0.23f, .label = "Disk I/O ", .width = 30}),
    });

    // Waterfall panel — build pipeline timing
    auto waterfall_panel = panel(" Build Pipeline ", p.border, {
        Waterfall({.entries = {
            {.label = "tsc compile",   .start = 0.0f, .duration = 1.2f,
             .color = p.info,    .status = TaskStatus::Completed},
            {.label = "lint",          .start = 0.0f, .duration = 0.8f,
             .color = p.accent,  .status = TaskStatus::Completed},
            {.label = "bundle",        .start = 1.2f, .duration = 0.6f,
             .color = p.primary, .status = TaskStatus::Completed},
            {.label = "auth.test",     .start = 1.8f, .duration = 0.9f,
             .color = tests_done ? p.success : p.warning,
             .status = tests_done ? TaskStatus::Completed : TaskStatus::InProgress},
            {.label = "token.test",    .start = 1.8f, .duration = 0.7f,
             .color = tests_done ? p.success : p.warning,
             .status = tests_done ? TaskStatus::Completed : TaskStatus::InProgress},
            {.label = "login.test",    .start = 2.0f, .duration = 0.5f,
             .color = tests_done ? p.success : p.warning,
             .status = tests_done ? TaskStatus::Completed : TaskStatus::Pending},
        }, .bar_width = 25, .frame = st.frame}),
    });

    return vstack().padding(0, 1, 0, 1).gap(1)(
        std::move(test_panel),
        std::move(results_panel),
        std::move(waterfall_panel),
        std::move(server_panel),
        std::move(metrics_panel)
    );
}

// ═══════════════════════════════════════════════════════════════════════════════
// Render: Config tab
// ═══════════════════════════════════════════════════════════════════════════════

static Element build_config(State& st) {
    auto& p = palette();

    auto model_panel = panel(" Model ", p.primary, {
        st.model_select.render(),
    });

    auto gen_panel = panel(" Generation ", p.border, {
        st.max_tokens.render(),
        st.temperature.render(),
        st.budget_input.render(),
    });

    auto behavior_panel = panel(" Behavior ", p.border, {
        section(p.primary, {
            text("Permissions", Style{}.with_bold().with_fg(p.text)),
            vstack().gap(1)(
                Toggle({.checked = st.auto_approve, .label = "Auto-approve edits",   .style = ToggleStyle::Switch}),
                Toggle({.checked = st.diff_preview, .label = "Show diff previews",   .style = ToggleStyle::Switch}),
                Toggle({.checked = st.run_tests,    .label = "Auto-run tests after edits", .style = ToggleStyle::Switch})
            ),
        }),
        section(p.accent, {
            text("Output", Style{}.with_bold().with_fg(p.text)),
            vstack().gap(1)(
                Toggle({.checked = st.streaming,    .label = "Stream responses",    .style = ToggleStyle::Checkbox}),
                Toggle({.checked = st.dark_mode,    .label = "Dark mode",           .style = ToggleStyle::Checkbox}),
                Toggle({.checked = st.verbose_logs, .label = "Verbose logging",     .style = ToggleStyle::Checkbox}),
                Toggle({.checked = st.telemetry,    .label = "Telemetry (disabled)", .style = ToggleStyle::Dot,
                        .disabled = true})
            ),
        }),
    });

    auto cost_panel = panel(" Session Cost ", p.border, {
        CostMeter({
            .input_tokens = st.total_input_tokens,
            .output_tokens = st.total_output_tokens,
            .cache_read = st.cache_read,
            .cache_write = st.cache_write,
            .cost = st.total_cost,
            .budget = 5.00,
            .model = "claude-opus-4-6",
        }),
    });

    auto context_panel = panel(" Context Window ", p.border, {
        ContextWindow({
            .segments = {
                {"System",   12400, p.info},
                {"Chat",     static_cast<int>(st.total_input_tokens * 0.6), p.secondary},
                {"Tools",    static_cast<int>(st.total_input_tokens * 0.3), p.accent},
                {"Output",   st.total_output_tokens, p.success},
                {"Cache",    st.cache_read, Color::rgb(100, 180, 100)},
            },
            .max_tokens = 200000,
            .width = 40,
        }),
    });

    return vstack().padding(0, 1, 0, 1).gap(1)(
        std::move(model_panel),
        std::move(gen_panel),
        std::move(behavior_panel),
        std::move(cost_panel),
        std::move(context_panel),
        Callout({
            .severity = st.phase == Phase::Complete ? Severity::Success : Severity::Info,
            .title = st.phase == Phase::Complete ? "Session Complete" : "Session Active",
            .body = st.phase == Phase::Complete
                ? "All tasks completed successfully. 3 files modified, 14 tests passing."
                : "Agent is working on the auth refactoring task...",
        }),
        KeyBindings({
            {.keys = "Tab", .label = "switch tab"},
            {.keys = "j/k", .label = "navigate"},
            {.keys = "Enter", .label = "select"},
            {.keys = "a/d/t/s/v", .label = "toggle"},
            {.keys = "q", .label = "quit"},
        })
    );
}

// ═══════════════════════════════════════════════════════════════════════════════
// Main layout
// ═══════════════════════════════════════════════════════════════════════════════

static Element build_ui(State& st) {
    auto& p = palette();

    // Build active panel
    Element main_panel;
    switch (st.main_tabs.active()) {
        case 0: main_panel = build_chat(st);   break;
        case 1: main_panel = build_files(st);  break;
        case 2: main_panel = build_tests(st);  break;
        case 3: main_panel = build_config(st); break;
        default: main_panel = text("?");       break;
    }

    // Phase info
    const char* phase_label = "Ready";
    Color phase_color = p.success;
    switch (st.phase) {
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
    if (st.phase > Phase::ReadFile) plan_done++;
    if (st.phase > Phase::EditAuth) plan_done++;
    if (st.phase > Phase::CreateToken) plan_done++;
    if (st.phase > Phase::EditTests) plan_done++;
    if (st.phase > Phase::RunTests) plan_done++;

    bool active = st.phase != Phase::Complete;

    // Elapsed
    int secs = static_cast<int>(st.elapsed);

    // Status bar with spinner when active
    std::string status_str = active
        ? std::string(spin(st.frame)) + " " + phase_label
        : std::string("✓ ") + phase_label;

    return vstack()(
        // Status bar
        StatusBar(StatusBarProps{.sections = {
            {.content = " maya", .color = p.primary, .bold = true},
            {.content = "claude-opus-4-6", .color = p.accent},
            {.content = status_str, .color = phase_color, .bold = true},
            {.content = fmt("%d/5 steps", plan_done), .color = plan_done == 5 ? p.success : p.muted},
            {.content = fmt("$%.3f / $5.00", st.total_cost),
             .color = st.total_cost > 0.5 ? p.warning : p.muted},
            {.content = fmt("%ds", secs), .color = p.dim},
        }}),

        // Tabs
        st.main_tabs.render(),

        // Main content area with sidebar on chat/tests tabs
        hstack().gap(0)(
            // Main content — fills remaining space
            vstack().grow(1).shrink(1)(std::move(main_panel)),

            // Sidebar — fixed width, won't shrink
            st.main_tabs.active() == 0 || st.main_tabs.active() == 2
                ? vstack().width(Dimension::fixed(32)).shrink(0)(build_sidebar(st))
                : text("")
        )
    );
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
            if (key(ev, 'q')) return false;

            // Tab switching
            if (st.main_tabs.update(ev)) return true;

            // Forward to active panel's components
            switch (st.main_tabs.active()) {
                case 0: // Chat
                    if (st.chat_scroll.update(ev)) return true;
                    if (st.test_log.update(ev)) return true;
                    // Toggle tool collapse with number keys
                    if (key(ev, '!') || (ctrl(ev, '1'))) { st.tool1_collapsed = !st.tool1_collapsed; return true; }
                    if (key(ev, '@') || (ctrl(ev, '2'))) { st.tool2_collapsed = !st.tool2_collapsed; return true; }
                    break;
                case 1: // Files
                    if (st.file_tree.update(ev)) return true;
                    if (st.deps_table.update(ev)) return true;
                    break;
                case 2: // Tests
                    if (st.test_log.update(ev)) return true;
                    if (st.server_log.update(ev)) return true;
                    break;
                case 3: // Config
                    if (st.model_select.update(ev)) return true;
                    if (st.max_tokens.update(ev)) return true;
                    if (st.temperature.update(ev)) return true;
                    if (st.budget_input.update(ev)) return true;
                    // Toggle shortcuts
                    if (key(ev, 'a')) { st.auto_approve = !st.auto_approve; return true; }
                    if (key(ev, 'd')) { st.diff_preview = !st.diff_preview; return true; }
                    if (key(ev, 't')) { st.run_tests = !st.run_tests; return true; }
                    if (key(ev, 's')) { st.streaming = !st.streaming; return true; }
                    if (key(ev, 'v')) { st.verbose_logs = !st.verbose_logs; return true; }
                    break;
            }

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
