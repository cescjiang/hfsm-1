/** @file   hfsm.c
 *  @brief  階層型有限状態マシン (HFSM: Hierarchical Finite State Machine) 実装.
 *
 *  @author t-kenji <protect.2501@gmail.com>
 *  @date   2018-03-18 新規作成.
 *  @copyright  Copyright (c) 2018 t-kenji
 *
 *  This code is licensed under the MIT License.
 */
#include <stdlib.h>
#include <errno.h>
#include <assert.h>

#include "debug.h"
#include "hfsm.h"

/**
 *  最大のコンポジット状態ネスト.
 */
#define NEST_MAX (5)

/**
 *  状態マシン構造体.
 */
struct fsm {
    const struct fsm_state *current;  /**< 現在の状態. */
    const struct fsm_trans *corresps; /**< 遷移の対応情報.*/

    STACK src_ancestors;              /**< 元状態の祖先を保持するバッファ. */
    STACK dest_ancestors;             /**< 先状態の祖先を保持するバッファ. */
};

/**
 *  状態マシン構造体の設定ヘルパ.
 */
#define FSM_HELPER(curr, corr, s, d) \
    (struct fsm){                    \
        .current = (curr),           \
        .corresps = (corr),          \
        .src_ancestors = (s),        \
        .dest_ancestors = (d)        \
    }

/**
 *  開始状態.
 */
const struct fsm_state state_start_ = FSM_STATE_INITIALIZER("start"),
                       *state_start = &state_start_;

/**
 *  終了状態.
 */
const struct fsm_state state_end_ = FSM_STATE_INITIALIZER("end"),
                      *state_end = &state_end_;

/**
 *  Null 遷移イベント.
 */
const struct fsm_event event_null_ = FSM_EVENT_INITIALIZER("null"),
                       *event_null = &event_null_;

/**
 *  指定する状態の変数を取得する.
 *
 *  呼び出し側の処理をシンプルにするため, 状態に変数が設定されていない場合も
 *  固定の空変数を返す.
 *
 *  @param  [in]    state   状態.
 *  @return 変数が設定されている場合は, @c state の変数のポインタが返る.
 *          設定されていない場合は, 固定の空変数のポインタが返る.
 *  @pre    @c state の非 NULL は呼び出し側で保証すること.
 */
static inline struct fsm_state_variable *get_state_variable(const struct fsm_state *state)
{
    static struct fsm_state_variable null_obj = FSM_STATE_VARIABLE_INITIALIZER;
    return (state->variable != NULL) ? state->variable : &null_obj;
}

/**
 *  entry アクションが設定されていれば, 実行する.
 *
 *  entry アクションが設定されていない場合は何もしない.
 *  状態が入れ子になっている場合, 目標となる子にたどり着く途中では
 *  @c cmpl が false となる.
 *
 *  @param  [in]    machine 状態マシン.
 *  @param  [in]    state   遷移先の状態.
 *  @param  [in]    cmpl    遷移完了.
 *  @pre    @c machine の非 NULL は呼び出し側で保証すること.
 *  @pre    @c state の非 NULL は呼び出し側で保証すること.
 */
static inline void entry_if_can_be(struct fsm *machine,
                                   const struct fsm_state *state,
                                   bool cmpl)
{
    if (state->entry != NULL) {
        state->entry(machine, get_state_variable(state)->data, cmpl);
    }
}

/**
 *  do アクティビティが設定されていれば, 実行する.
 *
 *  do アクティビティが設定されていない場合は何もしない.
 *
 *  @param  [in]    machine 状態マシン.
 *  @pre    @c machine の非 NULL は呼び出し側で保証すること.
 */
static inline void exec_if_can_be(struct fsm *machine)
{
    const struct fsm_state *state = machine->current;
    if ((state->exec != NULL)) {
        state->exec(machine, get_state_variable(state)->data);
    }
}

/**
 *  exit アクションが設定されていれば, 実行する.
 *
 *  exit アクションが設定されていない場合は何もしない.
 *  親状態の履歴状態の更新も行う.
 *  状態が入れ子になっている場合, 目標となる子にたどり着く途中では
 *  @c cmpl が false となる.
 *
 *  @param  [in]    machine 状態マシン.
 *  @param  [in]    state   遷移元の状態.
 *  @param  [in]    cmpl    遷移完了.
 *  @pre    @c machine の非 NULL は呼び出し側で保証すること.
 *  @pre    @c state の非 NULL は呼び出し側で保証すること.
 */
static inline void exit_if_can_be(struct fsm *machine,
                                  const struct fsm_state *state,
                                  bool cmpl)
{
    const struct fsm_state *parent = get_state_variable(state)->parent;

    if (state->exit != NULL) {
        state->exit(machine, get_state_variable(state)->data, cmpl);
    }
    if (parent != NULL) {
        get_state_variable(parent)->history = state;
    }
}

/**
 *  状態マシンの状態を変更する.
 *
 *  @param  [in,out]    machine     状態マシン.
 *  @param  [in]        new_state   新しい状態.
 *  @pre    @c machine は非 NULL は呼び出し側で保証すること.
 *  @pre    @c new_state は非 NULL は呼び出し側で保証すること.
 */
static void fsm_change_state(struct fsm *machine,
                             const struct fsm_state *new_state)
{
    STACK src_ancs = machine->src_ancestors;
    STACK dest_ancs = machine->dest_ancestors;
    const struct fsm_state *src_state;
    const struct fsm_state *dest_state;
    const struct fsm_state *ancestor;
    int count;

    /* 自己遷移の場合 */
    if (machine->current == new_state) {
        exit_if_can_be(machine, machine->current, true);
        entry_if_can_be(machine, new_state, true);
        return;
    }

    /* 共通の祖先より後の出状/入状イベントを処理する.
     * e.g. 以下の場合は, f,e,d の出状イベントと g,h,i の入場イベントを処理する.
     *      a--b--c--d--e--f(現在状態)
     *             \-g--h--i(次の状態)
     */
    stack_clear(src_ancs);
    for (src_state = machine->current;
         src_state != NULL;
         src_state = get_state_variable(src_state)->parent) {

        stack_push(src_ancs, (void *)&src_state);
    }
    stack_clear(dest_ancs);
    for (dest_state = new_state;
         dest_state != NULL;
         dest_state = get_state_variable(dest_state)->parent) {

        stack_push(dest_ancs, (void *)&dest_state);
    }
    do {
        src_state = dest_state = NULL;
        stack_pop(src_ancs, &src_state);
        count = stack_pop(dest_ancs, &dest_state);
    } while (src_state == dest_state);
    assert(dest_state != NULL);
    ancestor = (src_state != NULL) ? get_state_variable(src_state)->parent : machine->current;

    for (src_state = machine->current;
         src_state != ancestor;
         src_state = get_state_variable(src_state)->parent) {

        assert(src_state != NULL);
        exit_if_can_be(machine, src_state, (get_state_variable(src_state)->parent == ancestor));
    }
    machine->current = new_state;
    do {
        entry_if_can_be(machine, dest_state, (count == 0));
        count = stack_pop(dest_ancs, &dest_state);
    } while (count >= 0);

    /* 履歴状態に対する遷移を行う. */
    if (get_state_variable(dest_state)->history != NULL) {
        fsm_change_state(machine, get_state_variable(dest_state)->history);
    }
}

/**
 *  状態の遷移を行う.
 *
 *  遷移にガード条件が設定されている場合は, 条件を満たさない場合は
 *  遷移は行わない.
 *  遷移にアクションが設定されている場合は, アクションを実行後に遷移を行う.
 *  遷移先が NULL の場合は内部遷移となる.
 *
 *  @param  [in]    machine 状態マシン.
 *  @param  [in]    state   起点となる状態.
 *  @param  [in]    event   発生したイベント.
 *  @return 対応表に条件が一致する項目があった場合は, 状態遷移が行われ, true が返る.
 *          一致する項目がなかった場合は, false が返る.
 *  @pre    @c machine の非 NULL は呼び出し側で保証すること.
 *  @pre    @c state の非 NULL は呼び出し側で保証すること.
 *  @pre    @c event の非 NULL は呼び出し側で保証すること.
 */
static bool fsm_state_transit(struct fsm *machine,
                              const struct fsm_state *state,
                              const struct fsm_event *event)
{
    int i;

    for (i = 0; machine->corresps[i].from != NULL; ++i) {
        const struct fsm_trans *corr = &machine->corresps[i];
        if ((corr->from == state) && (corr->event == event)) {
            if ((corr->cond == NULL) || corr->cond->func(machine)) {
                if (corr->action != NULL) {
                    corr->action->func(machine);
                }
if (corr->to == NULL) {
    if ((corr->cond == NULL) && (corr->action == NULL)) {
        DEBUG("state: %s %s", corr->from->name, corr->event->name);
    } else if (corr->action == NULL) {
        DEBUG("state: %s %s[%s]", corr->from->name, corr->event->name, corr->cond->name);
    } else if (corr->cond == NULL) {
        DEBUG("state: %s %s/%s", corr->from->name, corr->event->name, corr->action->name);
    } else {
        DEBUG("state: %s %s[%s]/%s", corr->from->name, corr->event->name, corr->cond->name, corr->action->name);
    }
} else {
    if ((corr->cond == NULL) && (corr->action == NULL)) {
        DEBUG("state: %s --%s-> %s", corr->from->name, corr->event->name, corr->to->name);
    } else if (corr->action == NULL) {
        DEBUG("state: %s --%s[%s]-> %s", corr->from->name, corr->event->name, corr->cond->name, corr->to->name);
    } else if (corr->cond == NULL) {
        DEBUG("state: %s --%s/%s-> %s", corr->from->name, corr->event->name, corr->action->name, corr->to->name);
    } else {
        DEBUG("state: %s --%s[%s]/%s-> %s", corr->from->name, corr->event->name, corr->cond->name, corr->action->name, corr->to->name);
    }
}
                if (corr->to != NULL) {
                    fsm_change_state(machine, corr->to);
                }

                return true;
            }
        }
    }

    return false;
}

/**
 *  @details    開始状態の状態マシンを, 生成する.
 *              初期化後の状態は @ref state_start となる.
 *
 *              @c rels が設定されている場合は, 指定に従って状態の親を設定する.
 *              状態マシンは, @c corresps の設定の状態遷移を行う.
 *
 *  @param      [in]    rels        状態の関係性.
 *  @param      [in]    corresps    状態遷移の対応表.
 *  @return     成功時は, 確保および初期化されたオブジェクトのポインタが返る.
 *              失敗時は, NULL が返り, errno が適切に設定される.
 */
struct fsm *fsm_init(const struct fsm_rels *rels,
                     const struct fsm_trans *corresps)
{
    struct fsm *machine;
    STACK src_ancs, dest_ancs;

    if (corresps == NULL) {
        errno = EINVAL;
        return NULL;
    }

    machine = malloc(sizeof(struct fsm));
    src_ancs = stack_init(sizeof(struct fsm_state*), NEST_MAX);
    dest_ancs = stack_init(sizeof(struct fsm_state*), NEST_MAX);
    if ((machine == NULL) || (src_ancs == NULL) || (dest_ancs == NULL)) {
        stack_release(dest_ancs);
        stack_release(src_ancs);
        free(machine);
        return NULL;
    }

    *machine = FSM_HELPER(state_start, corresps, src_ancs, dest_ancs);

    /* 状態の関係性を設定する. */
    if (rels != NULL) {
        const struct fsm_state *oneself, *parent;
        for (int i = 0; rels[i].oneself != NULL; ++i) {
            oneself = rels[i].oneself;
            parent = rels[i].parent;
            get_state_variable(oneself)->parent = parent;
            if (rels[i].is_default) {
                get_state_variable(parent)->history = oneself;
            }
        }
    }

    /* Null 遷移を行う. */
    fsm_state_transit(machine, machine->current, event_null);

    return machine;
}

/**
 *  @details    @c machine の使用領域を解放する.
 *
 *  @param      [in,out]    machine 状態マシン.
 *  @return     成功時は, 0 が返る.
 *              失敗時は, -1 が返り, errno が適切に設定される.
 */
int fsm_term(struct fsm *machine)
{
    if (machine == NULL) {
        errno = EINVAL;
        return -1;
    }

    fsm_change_state(machine, state_end);
    stack_release(machine->dest_ancestors);
    stack_release(machine->src_ancestors);
    free(machine);

    return 0;
}

/**
 *  @details    指定イベントによる状態遷移を発生させる.
 *              現在の状態に対応する遷移がない場合は, 親にイベントを伝播させる.
 *
 *  @param      [in]    machine 状態マシン.
 *  @param      [in]    event   発生したイベント.
 */
void fsm_transition(struct fsm *machine, const struct fsm_event *event)
{
    const struct fsm_state *state;

    if ((machine == NULL) || (event == NULL)) {
        return;
    }

    state = machine->current;
    while ((state != NULL) && !fsm_state_transit(machine, state, event)) {
        state = get_state_variable(state)->parent;
    }

    /* Null 遷移を行う. */
    fsm_state_transit(machine, machine->current, event_null);
}

/**
 *  @details    現在の状態の do アクティビティを実行する.
 *
 *  @param  [in]    machine 状態マシン.
 */
void fsm_update(struct fsm *machine)
{
    if (machine == NULL) {
        return;
    }

    exec_if_can_be(machine);
}

/**
 *  @details    指定状態の固有情報を取得する.
 *
 *  @param      [in]    state   固有情報を取得したい状態.
 *  @return     正常時は, 固有情報のポインタが返る.
 *              失敗時は, NULL が返り, errno が適切に設定される.
 */
void *fsm_get_state_data(const struct fsm_state *state)
{
    if (state == NULL) {
        errno = EINVAL;
        return NULL;
    }

    return get_state_variable(state)->data;
}

/**
 *  @details    現在の状態の状態名を指定されたバッファにコピーする.
 *              コピーする文字列は NULL 終端を保証し, 
 *              実際にコピー可能な文字数は @c len - 1 文字となる.
 *
 *  @param      [in]    machine 状態マシン.
 *  @param      [out]   name    取得するバッファ.
 *  @param      [in]    len     取得する文字数.
 */
void fsm_current_state(struct fsm *machine, char *name, size_t len)
{
    if ((machine == NULL) || (name == NULL) || (len == 0)) {
        return;
    }

    strncpy(name, machine->current->name, len);
    name[len - 1] = '\0';
}

/**
 *  @details    @c machine の状態遷移を収集し, 収集した @ref TREE を @c handler
 *              に渡す.
 *              @c handler で任意フォーマットに出力すること.
 *
 *  @param      [in]    machine 状態マシン.
 *  @param      [in]    handler 出力処理のハンドラ.
 */
void fsm_dump_state_transition(struct fsm *machine, void (*handler)(TREE))
{
    SET states;
    const struct fsm_state *state;
    TREE tree;
    QUEUE reserve;

    if ((machine == NULL) || (handler == NULL)) {
        return;
    }

    /* すべての状態を収集する. */
    states = set_init(sizeof(struct fsm_state *), 30);
    for (int i = 0; machine->corresps[i].from != NULL; ++i) {
        const struct fsm_trans *corr = &machine->corresps[i];
        for (state = corr->from; state != NULL; state = get_state_variable(state)->parent) {
            set_add(states, (void *)&state);
        }
        for (state = corr->to; state != NULL; state = get_state_variable(state)->parent) {
            set_add(states, (void *)&state);
        }
    }

    /* 状態の親子関係を多分木に変換する.
     * 収集した状態は順不同であり, 多分木に追加する際に必ず親が追加されている
     * とは限らない.
     * その為, 追加に失敗した要素はキューに追加しておき, リトライする.
     */
    tree = tree_init(sizeof(struct fsm_state *), set_count(states));
    reserve = queue_init(sizeof(struct fsm_state *), set_count(states));
    for (ITER it = set_iter(states); it != NULL; it = iter_next(it)) {
        state = *(const struct fsm_state **)iter_get_payload(it);
        const struct fsm_state **parent =
            (get_state_variable(state)->parent != NULL) ? &get_state_variable(state)->parent : NULL;
        if (tree_insert(tree, (void *)parent, (void *)&state) == NULL) {
            queue_enq(reserve, (void *)&state);
        }
    }
    set_release(states);
    while (queue_count(reserve) > 0) {
        if (queue_deq(reserve, &state) < 0) {
            break;
        }
        const struct fsm_state **parent =
            (get_state_variable(state)->parent != NULL) ? &get_state_variable(state)->parent : NULL;
        if (tree_insert(tree, (void *)parent, (void *)&state) == NULL) {
            queue_enq(reserve, (void *)&state);
        }
    }
    queue_release(reserve);

    handler(tree);

    tree_release(tree);
}
