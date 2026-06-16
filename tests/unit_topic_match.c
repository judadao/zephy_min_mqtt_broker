/*
 * Unit tests for MQTT topic wildcard matching logic.
 * The production topic_match() in topic.c is static; we mirror it here
 * to test in isolation without pulling in mutex/client dependencies.
 */
#include <stdio.h>
#include <string.h>

static int g_pass = 0;
static int g_fail = 0;

#define ASSERT(cond, msg) do { \
    if (cond) { printf("  [PASS] %s\n", msg); g_pass++; } \
    else       { printf("  [FAIL] %s  (line %d)\n", msg, __LINE__); g_fail++; } \
} while (0)

/* mirrors src/topic.c:topic_match(); keep in sync with production code */
static int topic_match(const char *filter, const char *topic)
{
    /* MQTT 3.1.1 §4.7.2: wildcards must not match topics starting with '$' */
    if (topic[0] == '$' && (filter[0] == '+' || filter[0] == '#')) {
        return 0;
    }

    const char *f = filter;
    const char *t = topic;

    while (*f && *t) {
        if (*f == '#') {
            return 1;
        }
        if (*f == '+') {
            while (*t && *t != '/') {
                t++;
            }
            f++;
        } else {
            if (*f != *t) {
                return 0;
            }
            f++;
            t++;
        }
    }

    if (*f == '#') {
        return 1;
    }
    /* MQTT 3.1.1 §4.7.1: "sport/tennis/#" matches "sport/tennis" */
    if (*f == '/' && *(f + 1) == '#' && *(f + 2) == '\0' && *t == '\0') {
        return 1;
    }
    return (*f == '\0' && *t == '\0');
}

static void test_exact(void)
{
    printf("\n--- exact match ---\n");
    ASSERT( topic_match("a/b/c",  "a/b/c"),  "a/b/c == a/b/c");
    ASSERT(!topic_match("a/b/c",  "a/b/d"),  "a/b/c != a/b/d");
    ASSERT(!topic_match("a/b/c",  "a/b"),    "a/b/c != a/b (shorter)");
    ASSERT(!topic_match("a/b",    "a/b/c"),  "a/b != a/b/c (longer)");
    ASSERT( topic_match("",       ""),        "empty == empty");
    ASSERT(!topic_match("",       "a"),       "empty != a");
    ASSERT( topic_match("a",      "a"),       "a == a");
}

static void test_single_level_wildcard(void)
{
    printf("\n--- single-level wildcard '+' ---\n");
    ASSERT( topic_match("+",       "a"),         "+ matches a");
    ASSERT( topic_match("+",       "abc"),        "+ matches abc");
    ASSERT(!topic_match("+",       "a/b"),        "+ does not match a/b");
    ASSERT( topic_match("a/+/c",   "a/b/c"),      "a/+/c matches a/b/c");
    ASSERT( topic_match("a/+/c",   "a/xyz/c"),    "a/+/c matches a/xyz/c");
    ASSERT(!topic_match("a/+/c",   "a/b/d"),      "a/+/c does not match a/b/d");
    ASSERT(!topic_match("a/+/c",   "a/b/c/d"),    "a/+/c does not match a/b/c/d");
    ASSERT( topic_match("+/+",     "a/b"),        "+/+ matches a/b");
    ASSERT(!topic_match("+/+",     "a"),           "+/+ does not match a");
    ASSERT( topic_match("sensor/+/temp", "sensor/A/temp"),  "sensor/+/temp matches A");
    ASSERT( topic_match("sensor/+/temp", "sensor/B/temp"),  "sensor/+/temp matches B");
    ASSERT(!topic_match("sensor/+/temp", "sensor/A/hum"),   "sensor/+/temp no hum");
}

static void test_multi_level_wildcard(void)
{
    printf("\n--- multi-level wildcard '#' ---\n");
    ASSERT( topic_match("#",       "a"),            "# matches a");
    ASSERT( topic_match("#",       "a/b/c"),         "# matches a/b/c");
    ASSERT( topic_match("#",       ""),              "# matches empty");
    ASSERT( topic_match("a/#",     "a/b"),           "a/# matches a/b");
    ASSERT( topic_match("a/#",     "a/b/c/d"),       "a/# matches a/b/c/d");
    ASSERT( topic_match("a/#",     "a"),             "a/# matches 'a' (MQTT 3.1.1 §4.7.1)");
    ASSERT(!topic_match("a/#",     "b/c"),           "a/# does not match b/c");
    ASSERT( topic_match("home/#",  "home/room/light"), "home/# matches home/room/light");
    ASSERT( topic_match("home/#",  "home/room/temp"),  "home/# matches home/room/temp");
    ASSERT(!topic_match("home/#",  "other/topic"),    "home/# does not match other/topic");
}

static void test_mixed_wildcards(void)
{
    printf("\n--- mixed wildcards ---\n");
    ASSERT( topic_match("+/#",     "a/b/c"),      "+/# matches a/b/c");
    ASSERT( topic_match("+/#",     "a/b"),         "+/# matches a/b");
    ASSERT( topic_match("a/+/#",   "a/b/c/d"),    "a/+/# matches a/b/c/d");
    ASSERT(!topic_match("a/+/#",   "b/c/d"),       "a/+/# does not match b/c/d");
}

static void test_edge_cases(void)
{
    printf("\n--- edge cases ---\n");
    ASSERT( topic_match("a",     "a"),    "single char exact");
    ASSERT(!topic_match("a",     "b"),    "single char mismatch");
    ASSERT( topic_match("+",     "a"),    "+ matches single char");
    ASSERT(!topic_match("+",     ""),     "+ does not match empty topic");
    /* $SYS topics: MQTT 3.1.1 §4.7.2 — wildcards must not match '$' prefix topics */
    ASSERT( topic_match("$SYS/#", "$SYS/broker"), "$SYS/# matches $SYS/broker");
    ASSERT(!topic_match("#",       "$SYS/broker"), "# does NOT match $SYS (§4.7.2)");
    ASSERT(!topic_match("+/broker","$SYS/broker"), "+ does NOT match $SYS first level (§4.7.2)");
}

int main(void)
{
    printf("=== unit_topic_match tests ===\n");

    test_exact();
    test_single_level_wildcard();
    test_multi_level_wildcard();
    test_mixed_wildcards();
    test_edge_cases();

    printf("\n=== Results: %d passed, %d failed ===\n", g_pass, g_fail);
    return (g_fail == 0) ? 0 : 1;
}
