#include <algorithm>
#include <unordered_map>
#include <utility>
#include <vector>
#include "111061143_state.hpp"
#include "111061143_submission.hpp"

namespace {

struct ScoredMove {
    Move move;
    int score;
};

enum TTFlag {
    TT_EXACT,
    TT_LOWER,
    TT_UPPER
};

struct TTEntry {
    int depth = -1;
    int score = 0;
    TTFlag flag = TT_EXACT;
    Move best_move{};
    bool has_best = false;
};

static thread_local std::unordered_map<uint64_t, TTEntry> tt;

static int piece_value(int piece){
    static const int value[7] = {0, 100, 500, 320, 330, 900, 20000};
    return (piece >= 0 && piece <= 6) ? value[piece] : 0;
}

static int move_order_score(State* state, const Move& move){
    int score = 0;
    int player = state->player;
    int opp = 1 - player;
    int from_r = static_cast<int>(move.first.first);
    int from_c = static_cast<int>(move.first.second);
    int to_r = static_cast<int>(move.second.first);
    int to_c = static_cast<int>(move.second.second);

    if(from_r >= 0 && from_r < state->board_h()
       && from_c >= 0 && from_c < state->board_w()
       && to_r >= 0 && to_r < state->board_h()
       && to_c >= 0 && to_c < state->board_w()){
        int moving = state->piece_at(player, from_r, from_c);
        int captured = state->piece_at(opp, to_r, to_c);
        if(captured){
            score += 10000 + 10 * piece_value(captured) - piece_value(moving);
        }
        if(moving == 1 && (to_r == 0 || to_r == state->board_h() - 1)){
            score += 8000;
        }

        int center_r2 = 2 * to_r - (state->board_h() - 1);
        int center_c2 = 2 * to_c - (state->board_w() - 1);
        score -= center_r2 * center_r2 + center_c2 * center_c2;
    }
    return score;
}

static bool same_move(const Move& a, const Move& b){
    return a.first == b.first && a.second == b.second;
}

static std::vector<Move> ordered_moves(State* state){
    Move tt_best{};
    bool has_tt_best = false;
    auto it = tt.find(state->hash());
    if(it != tt.end() && it->second.has_best){
        tt_best = it->second.best_move;
        has_tt_best = true;
    }

    std::vector<ScoredMove> scored;
    scored.reserve(state->legal_actions.size());
    for(const auto& move : state->legal_actions){
        int score = move_order_score(state, move);
        if(has_tt_best && same_move(move, tt_best)){
            score += 1000000;
        }
        scored.push_back({move, score});
    }
    std::stable_sort(scored.begin(), scored.end(),
        [](const ScoredMove& a, const ScoredMove& b){
            return a.score > b.score;
        });

    std::vector<Move> moves;
    moves.reserve(scored.size());
    for(const auto& sm : scored){
        moves.push_back(sm.move);
    }
    return moves;
}

static bool tt_probe(uint64_t key, int depth, int alpha, int beta, int& out){
    auto it = tt.find(key);
    if(it == tt.end()){
        return false;
    }
    const TTEntry& entry = it->second;
    if(entry.depth < depth){
        return false;
    }
    if(entry.flag == TT_EXACT){
        out = entry.score;
        return true;
    }
    if(entry.flag == TT_LOWER && entry.score >= beta){
        out = entry.score;
        return true;
    }
    if(entry.flag == TT_UPPER && entry.score <= alpha){
        out = entry.score;
        return true;
    }
    return false;
}

static void tt_store(
    uint64_t key,
    int depth,
    int score,
    int alpha_orig,
    int beta,
    const Move& best_move,
    bool has_best
){
    TTEntry& entry = tt[key];
    if(entry.depth > depth){
        return;
    }
    entry.depth = depth;
    entry.score = score;
    entry.has_best = has_best;
    if(has_best){
        entry.best_move = best_move;
    }
    if(score <= alpha_orig){
        entry.flag = TT_UPPER;
    }else if(score >= beta){
        entry.flag = TT_LOWER;
    }else{
        entry.flag = TT_EXACT;
    }
}

static bool is_noisy_move(State* state, const Move& move){
    int player = state->player;
    int opp = 1 - player;
    int from_r = static_cast<int>(move.first.first);
    int from_c = static_cast<int>(move.first.second);
    int to_r = static_cast<int>(move.second.first);
    int to_c = static_cast<int>(move.second.second);
    if(from_r < 0 || from_r >= state->board_h()
       || from_c < 0 || from_c >= state->board_w()
       || to_r < 0 || to_r >= state->board_h()
       || to_c < 0 || to_c >= state->board_w()){
        return false;
    }
    int moving = state->piece_at(player, from_r, from_c);
    if(state->piece_at(opp, to_r, to_c)){
        return true;
    }
    return moving == 1 && (to_r == 0 || to_r == state->board_h() - 1);
}

static int quiescence(
    State* state,
    int alpha,
    int beta,
    GameHistory& history,
    int ply,
    SearchContext& ctx,
    const MMParams& p
){
    ctx.nodes++;
    if(ply > ctx.seldepth){
        ctx.seldepth = ply;
    }
    if(ctx.stop){
        return 0;
    }

    if(state->legal_actions.empty() && state->game_state == UNKNOWN){
        state->get_legal_actions();
    }

    if(state->game_state == WIN){
        return P_MAX - ply;
    }
    if(state->game_state == DRAW){
        return -p.draw_contempt;
    }

    int rep_score;
    if(state->check_repetition(history, rep_score)){
        return rep_score == 0 ? -p.draw_contempt : rep_score;
    }

    int stand_pat = state->evaluate(p.use_kp_eval, p.use_eval_mobility, &history);
    if(stand_pat >= beta){
        return beta;
    }
    if(stand_pat > alpha){
        alpha = stand_pat;
    }

    history.push(state->hash());
    for(const auto& action : ordered_moves(state)){
        if(!is_noisy_move(state, action)){
            continue;
        }
        State* next = state->next_state(action);
        bool same = next->same_player_as_parent();
        int raw = quiescence(next, same ? alpha : -beta, same ? beta : -alpha,
            history, ply + 1, ctx, p);
        int score = same ? raw : -raw;
        delete next;

        if(score >= beta){
            history.pop(state->hash());
            return beta;
        }
        if(score > alpha){
            alpha = score;
        }
    }
    history.pop(state->hash());
    return alpha;
}

static int alphabeta_eval(
    State* state,
    int depth,
    int alpha,
    int beta,
    GameHistory& history,
    int ply,
    SearchContext& ctx,
    const MMParams& p
){
    ctx.nodes++;
    if(ply > ctx.seldepth){
        ctx.seldepth = ply;
    }
    if(ctx.stop){
        return 0;
    }

    if(state->legal_actions.empty() && state->game_state == UNKNOWN){
        state->get_legal_actions();
    }

    if(state->game_state == WIN){
        return P_MAX - ply;
    }
    if(state->game_state == DRAW){
        return -p.draw_contempt;
    }

    int rep_score;
    if(state->check_repetition(history, rep_score)){
        return rep_score == 0 ? -p.draw_contempt : rep_score;
    }

    if(depth <= 0){
        return quiescence(state, alpha, beta, history, ply, ctx, p);
    }

    uint64_t key = state->hash();
    int cached = 0;
    if(tt_probe(key, depth, alpha, beta, cached)){
        return cached;
    }

    history.push(state->hash());
    int alpha_orig = alpha;
    int best_score = M_MAX;
    Move best_move{};
    bool has_best = false;
    for(const auto& action : ordered_moves(state)){
        State* next = state->next_state(action);
        bool same = next->same_player_as_parent();
        int raw = alphabeta_eval(next, depth - 1,
            same ? alpha : -beta,
            same ? beta : -alpha,
            history, ply + 1, ctx, p);
        int score = same ? raw : -raw;
        delete next;

        if(score > best_score){
            best_score = score;
            best_move = action;
            has_best = true;
        }
        if(score > alpha){
            alpha = score;
        }
        if(alpha >= beta){
            break;
        }
    }
    history.pop(state->hash());
    tt_store(key, depth, best_score, alpha_orig, beta, best_move, has_best);
    return best_score;
}

static int pvs_eval(
    State* state,
    int depth,
    int alpha,
    int beta,
    GameHistory& history,
    int ply,
    SearchContext& ctx,
    const MMParams& p
){
    ctx.nodes++;
    if(ply > ctx.seldepth){
        ctx.seldepth = ply;
    }
    if(ctx.stop){
        return 0;
    }

    if(state->legal_actions.empty() && state->game_state == UNKNOWN){
        state->get_legal_actions();
    }

    if(state->game_state == WIN){
        return P_MAX - ply;
    }
    if(state->game_state == DRAW){
        return -p.draw_contempt;
    }

    int rep_score;
    if(state->check_repetition(history, rep_score)){
        return rep_score == 0 ? -p.draw_contempt : rep_score;
    }

    if(depth <= 0){
        return quiescence(state, alpha, beta, history, ply, ctx, p);
    }

    uint64_t key = state->hash();
    int cached = 0;
    if(tt_probe(key, depth, alpha, beta, cached)){
        return cached;
    }

    history.push(state->hash());
    int alpha_orig = alpha;
    int best_score = M_MAX;
    Move best_move{};
    bool has_best = false;
    bool first = true;

    for(const auto& action : ordered_moves(state)){
        State* next = state->next_state(action);
        bool same = next->same_player_as_parent();
        int score;

        if(first){
            int raw = pvs_eval(next, depth - 1,
                same ? alpha : -beta,
                same ? beta : -alpha,
                history, ply + 1, ctx, p);
            score = same ? raw : -raw;
            first = false;
        }else{
            int raw = pvs_eval(next, depth - 1,
                same ? alpha : -alpha - 1,
                same ? alpha + 1 : -alpha,
                history, ply + 1, ctx, p);
            score = same ? raw : -raw;
            if(score > alpha && score < beta){
                raw = pvs_eval(next, depth - 1,
                    same ? alpha : -beta,
                    same ? beta : -alpha,
                    history, ply + 1, ctx, p);
                score = same ? raw : -raw;
            }
        }

        delete next;

        if(score > best_score){
            best_score = score;
            best_move = action;
            has_best = true;
        }
        if(score > alpha){
            alpha = score;
        }
        if(alpha >= beta){
            break;
        }
    }

    history.pop(state->hash());
    tt_store(key, depth, best_score, alpha_orig, beta, best_move, has_best);
    return best_score;
}

static SearchResult root_search(
    State* state,
    int depth,
    GameHistory& history,
    SearchContext& ctx,
    bool use_pvs
){
    ctx.reset();
    if(depth <= 1){
        tt.clear();
        tt.reserve(1 << 20);
    }
    MMParams p = MMParams::from_map(ctx.params);
    SearchResult result;
    result.depth = depth;

    if(!state->legal_actions.size()){
        state->get_legal_actions();
    }

    int alpha = M_MAX;
    int beta = P_MAX;
    int best_score = M_MAX;
    int move_index = 0;
    int total_moves = static_cast<int>(state->legal_actions.size());
    bool first = true;

    for(const auto& action : ordered_moves(state)){
        State* next = state->next_state(action);
        bool same = next->same_player_as_parent();
        int score;

        if(use_pvs && !first){
            int raw = pvs_eval(next, depth - 1,
                same ? alpha : -alpha - 1,
                same ? alpha + 1 : -alpha,
                history, 1, ctx, p);
            score = same ? raw : -raw;
            if(score > alpha && score < beta){
                raw = pvs_eval(next, depth - 1,
                    same ? alpha : -beta,
                    same ? beta : -alpha,
                    history, 1, ctx, p);
                score = same ? raw : -raw;
            }
        }else{
            int raw = use_pvs
                ? pvs_eval(next, depth - 1,
                    same ? alpha : -beta,
                    same ? beta : -alpha,
                    history, 1, ctx, p)
                : alphabeta_eval(next, depth - 1,
                    same ? alpha : -beta,
                    same ? beta : -alpha,
                    history, 1, ctx, p);
            score = same ? raw : -raw;
        }

        first = false;
        delete next;

        if(score > best_score){
            best_score = score;
            result.best_move = action;
            if(p.report_partial && ctx.on_root_update){
                ctx.on_root_update({result.best_move, best_score, depth, move_index + 1, total_moves});
            }
        }
        if(score > alpha){
            alpha = score;
        }
        move_index++;
    }

    result.score = best_score;
    result.seldepth = ctx.seldepth;
    result.nodes = ctx.nodes;
    result.pv = {result.best_move};
    return result;
}

} // namespace


/*============================================================
 * MiniMax — eval_ctx
 *
 * Negamax without pruning. Caller manages memory.
 *============================================================*/
int MiniMax::eval_ctx(
    State *state,
    int depth,
    GameHistory& history,
    int ply,
    SearchContext& ctx,
    const MMParams& p
){
    ctx.nodes++;
    if(ply > ctx.seldepth){
        ctx.seldepth = ply;
    }
    if(ctx.stop){
        return 0;
    }

    /* === Lazy move generation (sets game_state) === */
    if(state->legal_actions.empty() && state->game_state == UNKNOWN){
        state->get_legal_actions();
    }

    /* === Terminal / leaf checks === */

    // [ Hackathon TODO 3-1 ]
    // return the score for a winning terminal state
    // Hint: prefer faster wins by using ply.
    if(state->game_state == WIN){
        return P_MAX - ply;
    }

    if(state->game_state == DRAW){
        return -p.draw_contempt;
    }

    /* === Repetition check (game-specific) === */
    int rep_score;
    if(state->check_repetition(history, rep_score)){
        return rep_score == 0 ? -p.draw_contempt : rep_score;
    }
    history.push(state->hash());

    if(depth <= 0){
        int score = state->evaluate(
            p.use_kp_eval, p.use_eval_mobility, &history
        ); 
        history.pop(state->hash());
        return score;
    }

    /* === Negamax loop === */
    int best_score = M_MAX;

    for(auto& action : state->legal_actions){
        // [ Hackathon TODO 3-2 ]
        // create the child state after applying action
        State* next = state->next_state(action);

        bool same = next->same_player_as_parent();

        // [Hackathon TODO 3-3]
        // search the child one level deeper
        int raw = eval_ctx(next, depth - 1, history, ply + 1, ctx, p);

        // [Hackathon TODO 3-4]
        // convert raw to the current player's perspective.
        int score = same ? raw : -raw;

        delete next;

        // [ Hackathon TODO 3-5 ]
        // update best_score if this child is better.
        if(score > best_score){
            best_score = score;
        }

    }

    history.pop(state->hash());
    return best_score;
}


/*============================================================
 * MiniMax — search
 *
 * Iterate legal moves, call eval_ctx, return SearchResult.
 *============================================================*/
SearchResult MiniMax::search(
    State *state,
    int depth,
    GameHistory& history,
    SearchContext& ctx
){
    ctx.reset();
    MMParams p = MMParams::from_map(ctx.params);
    SearchResult result;
    result.depth = depth;

    if(!state->legal_actions.size()){
        state->get_legal_actions();
    }


    int best_score = M_MAX - 10;
    int move_index = 0;
    int total_moves = (int)state->legal_actions.size();

    for(auto& action : state->legal_actions){
        /* [ Hackathon TODO 4-1 ]
         * search this move like TODO 3, but starting from the root */
        State* next = state->next_state(action);
        bool same = next->same_player_as_parent();
        int raw = eval_ctx(next, depth - 1, history, 1, ctx, p);
        int score = same ? raw : -raw;
        delete next;

            if(score > best_score){
                // [ Hackathon TODO 4-2 ]
                // keep this move if it is the best so far
                best_score = score;
                result.best_move = action;

                if(p.report_partial && ctx.on_root_update){
                   ctx.on_root_update({result.best_move, best_score, depth, move_index + 1, total_moves});
                }
            }  
        move_index++;
    }

    // [ Hackathon TODO 4-3 ]
    // update result and return
    result.score = best_score;
    result.seldepth = ctx.seldepth;
    result.nodes = ctx.nodes;
    result.pv = {result.best_move};

        return result;
} 


/*============================================================
 * MiniMax — default_params / param_defs
 *============================================================*/
ParamMap MiniMax::default_params(){
    return {
        {"UseKPEval", "true"},
        {"UseEvalMobility", "true"},
        {"ReportPartial", "true"},
        {"DrawContempt", "20"},
    };
}

std::vector<ParamDef> MiniMax::param_defs(){
    return {
        {"UseKPEval", ParamDef::CHECK, "true"},
        {"UseEvalMobility", ParamDef::CHECK, "true"},
        {"ReportPartial", ParamDef::CHECK, "true"},
        {"DrawContempt", ParamDef::SPIN, "20", 0, 200},
    };
}


/*============================================================
 * AlphaBeta / PVS — stronger searches over the same evaluation
 *============================================================*/
SearchResult AlphaBeta::search(
    State *state,
    int depth,
    GameHistory& history,
    SearchContext& ctx
){
    return root_search(state, depth, history, ctx, false);
}

ParamMap AlphaBeta::default_params(){
    return MiniMax::default_params();
}

std::vector<ParamDef> AlphaBeta::param_defs(){
    return MiniMax::param_defs();
}

SearchResult PVS::search(
    State *state,
    int depth,
    GameHistory& history,
    SearchContext& ctx
){
    return root_search(state, depth, history, ctx, true);
}

ParamMap PVS::default_params(){
    return MiniMax::default_params();
}

std::vector<ParamDef> PVS::param_defs(){
    return MiniMax::param_defs();
}
