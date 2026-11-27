// Runs on your computer, no board needed:  pio test -e native
#include <string.h>
#include <unity.h>

#include "activity.h"

static const char* kMe = "G2aEGJFQYQUQPMmUT7RAnqFBX3hvHjENQ1GcUQDZ8EPB";
static const char* kFriend = "C77JeUzT4mWdRk2LxVbP9fHcQe6NaZ3tYp8GNo8s1111";
static const char* kUsdc = "EPjFWdd5AufqSSqeM2qN1xzybapC8G4wEGGkZwyTDt1v";

// getSignaturesForAddress, newest first (trimmed: the real one adds slot, memo, status).
static const char* kSigs = R"({"jsonrpc":"2.0","id":3,"result":[
  {"signature":"sigSwap","blockTime":1764260000,"err":null,"slot":5},
  {"signature":"sigRecv","blockTime":1764250000,"err":null},
  {"signature":"sigSend","blockTime":1764240000,"err":null},
  {"signature":"sigFail","blockTime":1764230000,"err":{"InstructionError":[0,"Custom"]}},
  {"signature":"sigApp","blockTime":1764220000,"err":null}]})";

// A batch answer: one {"id","result"} per question, in any order. Trimmed getTransaction
// (jsonParsed) answers; the extra fields (logMessages, signer...) are what the filter drops.
static const char* kDetails = R"([
 {"jsonrpc":"2.0","id":2,"result":{"blockTime":1764240000,"slot":9,
   "meta":{"err":null,"fee":5000,"preBalances":[2000000000,0,1],"postBalances":[1499995000,500000000,1],
     "preTokenBalances":[],"postTokenBalances":[],"logMessages":["Program 111 invoke [1]","Program 111 success"]},
   "transaction":{"signatures":["sigSend"],"message":{
     "accountKeys":[{"pubkey":"G2aEGJFQYQUQPMmUT7RAnqFBX3hvHjENQ1GcUQDZ8EPB","signer":true,"writable":true,"source":"transaction"},
                    {"pubkey":"C77JeUzT4mWdRk2LxVbP9fHcQe6NaZ3tYp8GNo8s1111","signer":false},
                    {"pubkey":"11111111111111111111111111111111"}],
     "instructions":[{"program":"system","programId":"11111111111111111111111111111111","stackHeight":null,
       "parsed":{"type":"transfer","info":{"source":"G2aEGJFQYQUQPMmUT7RAnqFBX3hvHjENQ1GcUQDZ8EPB",
         "destination":"C77JeUzT4mWdRk2LxVbP9fHcQe6NaZ3tYp8GNo8s1111","lamports":500000000}}}]}}}},
 {"jsonrpc":"2.0","id":1,"result":{"blockTime":1764250000,
   "meta":{"err":null,"fee":5000,"preBalances":[900000000,2039280,2039280,1],"postBalances":[899995000,2039280,2039280,1],
     "preTokenBalances":[
       {"accountIndex":1,"mint":"EPjFWdd5AufqSSqeM2qN1xzybapC8G4wEGGkZwyTDt1v","owner":"C77JeUzT4mWdRk2LxVbP9fHcQe6NaZ3tYp8GNo8s1111",
        "uiTokenAmount":{"amount":"100000000","decimals":6,"uiAmount":100.0,"uiAmountString":"100"}},
       {"accountIndex":2,"mint":"EPjFWdd5AufqSSqeM2qN1xzybapC8G4wEGGkZwyTDt1v","owner":"G2aEGJFQYQUQPMmUT7RAnqFBX3hvHjENQ1GcUQDZ8EPB",
        "uiTokenAmount":{"uiAmountString":"5"}}],
     "postTokenBalances":[
       {"accountIndex":1,"mint":"EPjFWdd5AufqSSqeM2qN1xzybapC8G4wEGGkZwyTDt1v","owner":"C77JeUzT4mWdRk2LxVbP9fHcQe6NaZ3tYp8GNo8s1111",
        "uiTokenAmount":{"uiAmountString":"75.5"}},
       {"accountIndex":2,"mint":"EPjFWdd5AufqSSqeM2qN1xzybapC8G4wEGGkZwyTDt1v","owner":"G2aEGJFQYQUQPMmUT7RAnqFBX3hvHjENQ1GcUQDZ8EPB",
        "uiTokenAmount":{"uiAmountString":"29.5"}}]},
   "transaction":{"message":{"accountKeys":[{"pubkey":"C77JeUzT4mWdRk2LxVbP9fHcQe6NaZ3tYp8GNo8s1111"},{"pubkey":"FrAta"},{"pubkey":"MyAta"},
     {"pubkey":"G2aEGJFQYQUQPMmUT7RAnqFBX3hvHjENQ1GcUQDZ8EPB"}],
     "instructions":[{"program":"spl-token","parsed":{"type":"transferChecked","info":{"source":"FrAta","destination":"MyAta"}}}]}}}},
 {"jsonrpc":"2.0","id":0,"result":{"blockTime":1764260000,
   "meta":{"err":null,"fee":5000,"preBalances":[3000000000,0],"postBalances":[1997955720,2039280],
     "preTokenBalances":[],
     "postTokenBalances":[{"accountIndex":1,"mint":"EPjFWdd5AufqSSqeM2qN1xzybapC8G4wEGGkZwyTDt1v","owner":"G2aEGJFQYQUQPMmUT7RAnqFBX3hvHjENQ1GcUQDZ8EPB",
        "uiTokenAmount":{"uiAmountString":"141.2"}}]},
   "transaction":{"message":{"accountKeys":[{"pubkey":"G2aEGJFQYQUQPMmUT7RAnqFBX3hvHjENQ1GcUQDZ8EPB"},{"pubkey":"MyNewAta"}],
     "instructions":[{"program":"spl-memo","parsed":"swap via Jupiter"}]}}}},
 {"jsonrpc":"2.0","id":4,"result":{"blockTime":1764220000,
   "meta":{"err":null,"fee":5000,"preBalances":[1000000000,1],"postBalances":[999995000,1],
     "preTokenBalances":[],"postTokenBalances":[]},
   "transaction":{"message":{"accountKeys":[{"pubkey":"G2aEGJFQYQUQPMmUT7RAnqFBX3hvHjENQ1GcUQDZ8EPB"},{"pubkey":"SomeApp"}],
     "instructions":[{"program":"unknown","programId":"SomeApp","data":"3Bxs"}]}}}}
])";

static void build(Activity* a) {
  JsonDocument doc;
  TEST_ASSERT_FALSE(deserializeJson(doc, kSigs));
  Activity none = {};
  TEST_ASSERT_EQUAL(4, activityFromSignatures(doc["result"], none, a));  // the failed one is known
  JsonDocument filter;
  activityDetailsFilter(filter);
  TEST_ASSERT_FALSE(deserializeJson(doc, kDetails, DeserializationOption::Filter(filter)));
  activityApplyDetails(doc.as<JsonVariantConst>(), kMe, a);
}

void test_signatures_start_pending_and_failed_is_known() {
  JsonDocument doc;
  TEST_ASSERT_FALSE(deserializeJson(doc, kSigs));
  Activity none = {}, a;
  TEST_ASSERT_EQUAL(4, activityFromSignatures(doc["result"], none, &a));
  TEST_ASSERT_EQUAL(5, a.count);
  TEST_ASSERT_EQUAL_STRING("sigSwap", a.items[0].sig);
  TEST_ASSERT_EQUAL(1764260000u, a.items[0].blockTime);
  TEST_ASSERT_TRUE(a.items[0].kind == TxKind::kPending);
  TEST_ASSERT_TRUE(a.items[3].kind == TxKind::kFailed);
}

void test_details_body_asks_only_pending_ones() {
  JsonDocument doc;
  TEST_ASSERT_FALSE(deserializeJson(doc, kSigs));
  Activity none = {}, a;
  activityFromSignatures(doc["result"], none, &a);
  char body[1600];
  activityDetailsBody(a, body, sizeof body);
  JsonDocument req;
  TEST_ASSERT_FALSE(deserializeJson(req, body));  // valid JSON
  TEST_ASSERT_EQUAL(4, req.as<JsonArray>().size());
  TEST_ASSERT_EQUAL(0, req[0]["id"].as<int>());
  TEST_ASSERT_EQUAL_STRING("getTransaction", req[0]["method"]);
  TEST_ASSERT_EQUAL_STRING("sigSwap", req[0]["params"][0]);
  TEST_ASSERT_EQUAL(0, req[0]["params"][1]["maxSupportedTransactionVersion"].as<int>());
  TEST_ASSERT_EQUAL(4, req[3]["id"].as<int>());  // id = place in the list (3 = failed, skipped)
}

void test_sent_sol_leaves_fee_out_and_finds_recipient() {
  Activity a;
  build(&a);
  const TxItem& t = a.items[2];
  TEST_ASSERT_TRUE(t.kind == TxKind::kSent);
  TEST_ASSERT_EQUAL_STRING("SOL", t.symbol);
  TEST_ASSERT_DOUBLE_WITHIN(1e-9, 0.5, t.amount);
  TEST_ASSERT_DOUBLE_WITHIN(1e-12, 0.000005, t.feeSol);
  TEST_ASSERT_EQUAL_STRING(kFriend, t.other);
}

void test_received_token_finds_sender_wallet() {
  Activity a;
  build(&a);
  const TxItem& t = a.items[1];
  TEST_ASSERT_TRUE(t.kind == TxKind::kReceived);
  TEST_ASSERT_EQUAL_STRING("USDC", t.symbol);
  TEST_ASSERT_DOUBLE_WITHIN(1e-9, 24.5, t.amount);
  TEST_ASSERT_EQUAL_STRING(kFriend, t.other);  // the owner, not their token account
  TEST_ASSERT_DOUBLE_WITHIN(1e-12, 0, t.feeSol);  // the sender paid it
}

void test_swap_counts_rent_as_part_of_sol_out() {
  Activity a;
  build(&a);
  const TxItem& t = a.items[0];
  TEST_ASSERT_TRUE(t.kind == TxKind::kSwap);
  TEST_ASSERT_EQUAL_STRING("USDC", t.symbol);
  TEST_ASSERT_DOUBLE_WITHIN(1e-9, 141.2, t.amount);
  TEST_ASSERT_EQUAL_STRING("SOL", t.outSymbol);
  TEST_ASSERT_DOUBLE_WITHIN(1e-6, 1.002039, t.outAmount);  // 1 SOL + rent for the USDC account
}

void test_fee_only_is_an_app_call() {
  Activity a;
  build(&a);
  TEST_ASSERT_TRUE(a.items[4].kind == TxKind::kApp);
  TEST_ASSERT_DOUBLE_WITHIN(1e-12, 0.000005, a.items[4].feeSol);
}

void test_known_transactions_keep_details() {
  Activity a;
  build(&a);
  // A new transaction arrives on top: only that one needs asking.
  const char* newer = R"([{"signature":"sigNew","blockTime":1764270000,"err":null},
    {"signature":"sigSwap","blockTime":1764260000,"err":null},
    {"signature":"sigRecv","blockTime":1764250000,"err":null}])";
  JsonDocument doc;
  TEST_ASSERT_FALSE(deserializeJson(doc, newer));
  Activity b;
  TEST_ASSERT_EQUAL(1, activityFromSignatures(doc.as<JsonVariantConst>(), a, &b));
  TEST_ASSERT_EQUAL(3, b.count);
  TEST_ASSERT_TRUE(b.items[0].kind == TxKind::kPending);
  TEST_ASSERT_TRUE(b.items[1].kind == TxKind::kSwap);
  TEST_ASSERT_DOUBLE_WITHIN(1e-9, 141.2, b.items[1].amount);
}

void test_missing_answer_stays_pending() {
  JsonDocument doc;
  TEST_ASSERT_FALSE(deserializeJson(doc, kSigs));
  Activity none = {}, a;
  activityFromSignatures(doc["result"], none, &a);
  TEST_ASSERT_FALSE(deserializeJson(doc, R"([{"jsonrpc":"2.0","id":0,"result":null}])"));
  activityApplyDetails(doc.as<JsonVariantConst>(), kMe, &a);
  TEST_ASSERT_TRUE(a.items[0].kind == TxKind::kPending);
}

void test_format_ago_and_short_address() {
  char s[24];
  formatAgo(1000, 990, s, sizeof s);
  TEST_ASSERT_EQUAL_STRING("now", s);
  formatAgo(10000, 10000 - 5 * 60, s, sizeof s);
  TEST_ASSERT_EQUAL_STRING("5m ago", s);
  formatAgo(100000, 100000 - 3 * 3600, s, sizeof s);
  TEST_ASSERT_EQUAL_STRING("3h ago", s);
  formatAgo(1000000, 1000000 - 2 * 86400, s, sizeof s);
  TEST_ASSERT_EQUAL_STRING("2d ago", s);
  formatAgo(1000, 0, s, sizeof s);
  TEST_ASSERT_EQUAL_STRING("", s);
  shortAddress(kMe, s, sizeof s);
  TEST_ASSERT_EQUAL_STRING("G2aE...8EPB", s);
  (void)kUsdc;
}

void setUp() {}
void tearDown() {}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_signatures_start_pending_and_failed_is_known);
  RUN_TEST(test_details_body_asks_only_pending_ones);
  RUN_TEST(test_sent_sol_leaves_fee_out_and_finds_recipient);
  RUN_TEST(test_received_token_finds_sender_wallet);
  RUN_TEST(test_swap_counts_rent_as_part_of_sol_out);
  RUN_TEST(test_fee_only_is_an_app_call);
  RUN_TEST(test_known_transactions_keep_details);
  RUN_TEST(test_missing_answer_stays_pending);
  RUN_TEST(test_format_ago_and_short_address);
  return UNITY_END();
}
