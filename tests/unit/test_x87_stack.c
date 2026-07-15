#include "../../src/translator/guest/x86_64/lower/x87_stack.h"
#include "test.h"

static int test_reset_and_anchor(void) {
    struct hl_x87_stack stack = {7, 1, 1};

    hl_x87_stack_reset(&stack);
    HL_CHECK(!hl_x87_stack_known(&stack));
    HL_CHECK(!hl_x87_stack_dirty(&stack));

    hl_x87_stack_anchor(&stack, 10);
    HL_CHECK(hl_x87_stack_known(&stack));
    HL_CHECK(!hl_x87_stack_dirty(&stack));
    HL_CHECK(hl_x87_stack_top(&stack) == 2);
    return EXIT_SUCCESS;
}

static int test_slots_and_wrapping(void) {
    struct hl_x87_stack stack;

    hl_x87_stack_anchor(&stack, 0);
    HL_CHECK(hl_x87_stack_slot(&stack, 0) == 0);
    HL_CHECK(hl_x87_stack_slot(&stack, 7) == 7);
    HL_CHECK(hl_x87_stack_slot(&stack, -1) == 7);

    hl_x87_stack_push(&stack);
    HL_CHECK(hl_x87_stack_top(&stack) == 7);
    HL_CHECK(hl_x87_stack_dirty(&stack));
    HL_CHECK(hl_x87_stack_slot(&stack, 1) == 0);

    hl_x87_stack_adjust(&stack, 3);
    HL_CHECK(hl_x87_stack_top(&stack) == 2);
    hl_x87_stack_adjust(&stack, -3);
    HL_CHECK(hl_x87_stack_top(&stack) == 7);
    return EXIT_SUCCESS;
}

static int test_materialize_and_drop(void) {
    struct hl_x87_stack stack;

    hl_x87_stack_anchor(&stack, 4);
    hl_x87_stack_adjust(&stack, 1);
    HL_CHECK(hl_x87_stack_dirty(&stack));
    hl_x87_stack_materialized(&stack);
    HL_CHECK(hl_x87_stack_known(&stack));
    HL_CHECK(!hl_x87_stack_dirty(&stack));
    HL_CHECK(hl_x87_stack_top(&stack) == 5);

    hl_x87_stack_push(&stack);
    hl_x87_stack_drop(&stack);
    HL_CHECK(!hl_x87_stack_known(&stack));
    HL_CHECK(!hl_x87_stack_dirty(&stack));
    return EXIT_SUCCESS;
}

static int test_unknown_transitions_are_inert(void) {
    struct hl_x87_stack stack;

    hl_x87_stack_reset(&stack);
    hl_x87_stack_push(&stack);
    hl_x87_stack_adjust(&stack, 6);
    HL_CHECK(!hl_x87_stack_known(&stack));
    HL_CHECK(!hl_x87_stack_dirty(&stack));
    HL_CHECK(hl_x87_stack_top(&stack) == 0);
    return EXIT_SUCCESS;
}

int main(void) {
    HL_CHECK(test_reset_and_anchor() == EXIT_SUCCESS);
    HL_CHECK(test_slots_and_wrapping() == EXIT_SUCCESS);
    HL_CHECK(test_materialize_and_drop() == EXIT_SUCCESS);
    HL_CHECK(test_unknown_transitions_are_inert() == EXIT_SUCCESS);
    return EXIT_SUCCESS;
}
