/* Author: Dina Youakim*/

#include <sbpl_adaptive/core/search/traplanner.h>


#define ARAMDP_STATEID2IND_AD STATEID2IND_SLOT1
#define ARA_AD_INCONS_LIST_ID 1

namespace sbpl {

static const char* SLOG = "search";
static const char* SELOG = "search.expansions";

TRAPlanner::TRAPlanner( DiscreteSpaceInformation* space, Heuristic* heuristic, bool bSearchForward)
:
   SBPLPlanner(),
    m_space(space),
    m_heur(heuristic),
    m_time_params(),
    m_initial_eps(1.0),
    m_final_eps(1.0),
    m_delta_eps(1.0),
    m_allow_partial_solutions(false),
    m_states(),
    m_start_state_id(-1),
    m_goal_state_id(-1),
    m_graph_to_search_map(),
    m_open(),
    m_incons(),
    m_curr_eps(1.0),
    m_iteration(1),
    m_call_number(0),
    m_last_start_state_id(-1),
    m_last_goal_state_id(-1),
    m_last_eps(1.0),
    m_expand_count_init(0),
    m_expand_count(0),
    m_search_time_init(clock::duration::zero()),
    m_search_time(clock::duration::zero()),
    m_satisfied_eps(std::numeric_limits<double>::infinity()),
    bforwardsearch(bSearchForward),
    bsearchuntilfirstsolution(false),
    MaxMemoryCounter(0)
{
    environment_ = space;

    m_time_params.bounded = true;
    m_time_params.improve = true;
    m_time_params.type = TimeParameters::TIME;
    m_time_params.max_expansions_init = 0;
    m_time_params.max_expansions = 0;
    m_time_params.max_allowed_time_init = clock::duration::zero();
    m_time_params.max_allowed_time = clock::duration::zero();
}

TRAPlanner::~TRAPlanner()
{
    for (TRAState* s : m_states) {
        delete s;
    }
}

enum ReplanResultCode
{
    SUCCESS = 0,
    PARTIAL_SUCCESS,
    START_NOT_SET,
    GOAL_NOT_SET,
    TIMED_OUT,
    EXHAUSTED_OPEN_LIST
};

int TRAPlanner::replan(const TimeParameters& params,std::vector<int>* solution,int* cost)
{
    ROS_DEBUG_NAMED(SLOG, "Find path to goal");

    if (m_start_state_id < 0) {
        ROS_ERROR_NAMED(SLOG, "Start state not set");
        return !START_NOT_SET;
    }
    if (m_goal_state_id < 0) {
        ROS_ERROR_NAMED(SLOG, "Goal state not set");
        return !GOAL_NOT_SET;
    }

    m_time_params = params;

    TRAState* start_state = getSearchState(m_start_state_id);
    TRAState* goal_state = getSearchState(m_goal_state_id);

    if (m_start_state_id != m_last_start_state_id) {
        ROS_DEBUG_NAMED(SLOG, "Reinitialize search");
        m_open.clear();
        m_incons.clear();
        ++m_call_number; // trigger state reinitializations

        reinitSearchState(start_state);
        reinitSearchState(goal_state);

        start_state->g = 0;
        start_state->f = computeKey(start_state);
        m_open.push(start_state);

        m_iteration = 1; // 0 reserved for "not closed on any iteration"

        m_expand_count_init = 0;
        m_search_time_init = clock::duration::zero();

        m_expand_count = 0;
        m_search_time = clock::duration::zero();

        m_curr_eps = m_initial_eps;

        m_satisfied_eps = std::numeric_limits<double>::infinity();

        m_last_start_state_id = m_start_state_id;
    }

    if (m_goal_state_id != m_last_goal_state_id) {
        ROS_DEBUG_NAMED(SLOG, "Refresh heuristics, keys, and reorder open list");
        recomputeHeuristics();
        reorderOpen();
        heuristicChanged();
        m_last_goal_state_id = m_goal_state_id;
    }

    auto start_time = clock::now();
    int num_expansions = 0;
    clock::duration elapsed_time = clock::duration::zero();

    int err;
    while (m_satisfied_eps > m_final_eps)
    {
        if (m_curr_eps == m_satisfied_eps) {
            if (!m_time_params.improve) {
                break;
            }
            // begin a new search iteration
            ++m_iteration;
            m_curr_eps -= m_delta_eps;
            m_curr_eps = std::max(m_curr_eps, m_final_eps);
            for (TRAState* s : m_incons) {
                s->incons = false;
                m_open.push(s);
            }
            reorderOpen();
            m_incons.clear();
            ROS_DEBUG_NAMED(SLOG, "Begin new search iteration %d with epsilon = %0.3f", m_iteration, m_curr_eps);
        }
        err = improvePath(start_time, goal_state, num_expansions, elapsed_time);
        if (m_curr_eps == m_initial_eps) {
            m_expand_count_init += num_expansions;
            m_search_time_init += elapsed_time;
        }
        if (err) {
            break;
        }
        ROS_DEBUG_NAMED(SLOG, "Improved solution");
        m_satisfied_eps = m_curr_eps;
    }

    m_search_time += elapsed_time;
    m_expand_count += num_expansions;

    if (m_satisfied_eps == std::numeric_limits<double>::infinity()) {
        if (m_allow_partial_solutions && !m_open.empty()) {
            TRAState* next_state = m_open.min();
            extractPath(next_state, *solution, *cost);
            return !SUCCESS;
        }
        return !err;
    }

    extractPath(goal_state, *solution, *cost);
    return !SUCCESS;
}

int TRAPlanner::replan( double allowed_time, std::vector<int>* solution)
{
    int cost;
    return replan(allowed_time, solution, &cost);
}

// decide whether to start the search from scratch
//
// if start changed
//     reset the search to its initial state
// if goal changed
//     reevaluate heuristics
//     reorder the open list
//
// case scenario_hasnt_changed (start and goal the same)
//   case have solution for previous epsilon
//       case epsilon lowered
//           reevaluate heuristics and reorder the open list
//       case epsilon raised
//           pass
//   case dont have solution
//       case epsilon lowered
//           reevaluate heuristics and reorder the open list
//       case epsilon raised
//           reevaluate heuristics and reorder the open list
// case scenario_changed
int TRAPlanner::replan( double allowed_time, std::vector<int>* solution, int* cost)
{
    TimeParameters tparams = m_time_params;
    if (tparams.max_allowed_time_init == tparams.max_allowed_time) {
        // NOTE/TODO: this may lead to awkward behavior, if the caller sets the
        // allowed time to the current repair time, the repair time will begin
        // to track the allowed time for further calls to replan. perhaps set
        // an explicit flag for using repair time or an indicator value as is
        // done with ReplanParams
        tparams.max_allowed_time_init = to_duration(allowed_time);
        tparams.max_allowed_time = to_duration(allowed_time);
    } else {
        tparams.max_allowed_time_init = to_duration(allowed_time);
        // note: retain original allowed improvement time
    }
    return replan(tparams, solution, cost);
}

int TRAPlanner::replan( std::vector<int>* solution, ReplanParams params)
{
    int cost;
    return replan(solution, params, &cost);
}

int TRAPlanner::replan( std::vector<int>* solution, ReplanParams params, int* cost)
{
    // note: if replan fails before internal time parameters are updated (this
    // happens if the start or goal has not been set), then the internal
    // epsilons may be affected by this set of ReplanParams for future calls to
    // replan where ReplanParams is not used and epsilon parameters haven't been
    // set back to their desired values.
    TimeParameters tparams;
    convertReplanParamsToTimeParams(params, tparams);
    return replan(tparams, solution, cost);
}


int TRAPlanner::set_goal(int goal_state_id)
{
    /*if (bforwardsearch) {
        if (SetSearchGoalState(goal_stateID, pSearchStateSpace_) != 1) {
            SBPL_ERROR("ERROR: failed to set search goal state\n");
            return 0;
        }
    }
    else {
        if (SetSearchStartState(goal_stateID, pSearchStateSpace_) != 1) {
            SBPL_ERROR("ERROR: failed to set search start state\n");
            return 0;
        }
    }

    return 1;*/
    m_goal_state_id = goal_state_id;
    return 1;
}

int TRAPlanner::set_start(int start_state_id)
{
    /*if (bforwardsearch) {
        if (SetSearchStartState(start_stateID, pSearchStateSpace_) != 1) {
            SBPL_ERROR("ERROR: failed to set search start state\n");
            return 0;
        }
    }
    else {
        if (SetSearchGoalState(start_stateID, pSearchStateSpace_) != 1) {
            SBPL_ERROR("ERROR: failed to set search goal state\n");
            return 0;
        }
    }

    return 1;*/
    m_start_state_id = start_state_id;
    return 1;
}

//TODO: change implementation to recompute expansion and heuristics if needed
void TRAPlanner::costs_changed(const StateChangeQuery& stateChange)
{
    ROS_ERROR_NAMED(SLOG,"costs_changed(..) reInit!");
    force_planning_from_scratch();
}


int TRAPlanner::force_planning_from_scratch()
{
    m_last_start_state_id = -1;
    m_last_goal_state_id = -1;
    return 0;
}

int TRAPlanner::set_search_mode(bool bSearchUntilFirstSolution)
{
    ROS_DEBUG_NAMED(SLOG,"planner: search mode set to %d\n", bSearchUntilFirstSolution);

    m_time_params.bounded = !bSearchUntilFirstSolution;
    return 0;
}

/// Force the planner to forget previous search efforts, begin from scratch,
/// and free all memory allocated by the planner during previous searches.
int TRAPlanner::force_planning_from_scratch_and_free_memory()
{
    force_planning_from_scratch();
    m_open.clear();
    m_graph_to_search_map.clear();
    m_graph_to_search_map.shrink_to_fit();
    for (TRAState* s : m_states) {
        delete s;
    }
    m_states.clear();
    m_states.shrink_to_fit();
    return 0;
}

void TRAPlanner::update_succs_of_changededges(std::vector<int>* succsIDV)
{

}

//TODO: implement!!!!!!!!!!!
/// \brief direct form of informing the search about the new edge costs
/// \param predsIDV array of predecessors of changed edges
/// \note this is used when the search is run backwards
void TRAPlanner::update_preds_of_changededges(std::vector<int>* predsIDV)
{

}


// Convert TimeParameters to ReplanParams. Uses the current epsilon values
// to fill in the epsilon fields.
void TRAPlanner::convertTimeParamsToReplanParams(const TimeParameters& t, ReplanParams& r) const
{
    r.max_time = to_seconds(t.max_allowed_time_init);
    r.return_first_solution = !t.bounded && !t.improve;
    if (t.max_allowed_time_init == t.max_allowed_time) {
        r.repair_time = -1.0;
    } else {
        r.repair_time = to_seconds(t.max_allowed_time);
    }

    r.initial_eps = m_initial_eps;
    r.final_eps = m_final_eps;
    r.dec_eps = m_delta_eps;
}

// Convert ReplanParams to TimeParameters. Sets the current initial, final, and
// delta eps from ReplanParams.
void TRAPlanner::convertReplanParamsToTimeParams(const ReplanParams& r, TimeParameters& t)
{
    t.type = TimeParameters::TIME;

    t.bounded = !r.return_first_solution;
    t.improve = !r.return_first_solution;

    t.max_allowed_time_init = to_duration(r.max_time);
    if (r.repair_time > 0.0) {
        t.max_allowed_time = to_duration(r.repair_time);
    } else {
        t.max_allowed_time = t.max_allowed_time_init;
    }

    m_initial_eps = r.initial_eps;
    m_final_eps = r.final_eps;
    m_delta_eps = r.dec_eps;
}

// Test whether the search has run out of time.
bool TRAPlanner::timedOut(int elapsed_expansions, const clock::duration& elapsed_time) const
{
    if (!m_time_params.bounded) {
        return false;
    }

    switch (m_time_params.type) {
    case TimeParameters::EXPANSIONS:
        if (m_satisfied_eps == std::numeric_limits<double>::infinity()) {
            return elapsed_expansions >= m_time_params.max_expansions_init;
        } else {
            return elapsed_expansions >= m_time_params.max_expansions;
        }
    case TimeParameters::TIME:
        if (m_satisfied_eps == std::numeric_limits<double>::infinity()) {
            return elapsed_time >= m_time_params.max_allowed_time_init;
        } else {
            return elapsed_time >= m_time_params.max_allowed_time;
        }
    default:
        ROS_ERROR_NAMED(SLOG, "Invalid timer type");
        return true;
    }

    return true;
}

// Expand states to improve the current solution until a solution within the
// current suboptimality bound is found, time runs out, or no solution exists.
int TRAPlanner::improvePath( const clock::time_point& start_time,TRAState* goal_state, int& elapsed_expansions, clock::duration& elapsed_time)
{
    std::vector<int> succs;
    std::vector<int> costs;
    while (!m_open.empty()) {
        TRAState* min_state = m_open.min();

        auto now = clock::now();
        elapsed_time = now - start_time;

        // path to goal found
        if (min_state->f >= goal_state->f || min_state == goal_state) {
            ROS_DEBUG_NAMED(SLOG, "Found path to goal");
            return SUCCESS;
        }

        if (timedOut(elapsed_expansions, elapsed_time)) {
            ROS_DEBUG_NAMED(SLOG, "Ran out of time");
            return TIMED_OUT;
        }

        ROS_DEBUG_NAMED(SELOG, "Expand state %d", min_state->state_id);

        m_open.pop();

        assert(min_state->iteration_closed != m_iteration);
        assert(min_state->g != INFINITECOST);

        min_state->iteration_closed = m_iteration;
        min_state->eg = min_state->g;

        expand(min_state);

        ++elapsed_expansions;
    }

    return EXHAUSTED_OPEN_LIST;
}

// Expand a state, updating its successors and placing them into OPEN, CLOSED,
// and TRAPlanner list appropriately.
void TRAPlanner::expand(TRAState* s)
{
    s->v = s->g;
    std::vector<int> succs;
    std::vector<int> costs;
    m_space->GetSuccs(s->state_id, &succs, &costs);

    ROS_DEBUG_NAMED(SELOG, "  %zu successors", succs.size());

    for (size_t sidx = 0; sidx < succs.size(); ++sidx) 
    {
        int succ_state_id = succs[sidx];
        int cost = costs[sidx];

        TRAState* succ_state = getSearchState(succ_state_id);
        reinitSearchState(succ_state);

        int new_cost = s->eg + cost;
        ROS_DEBUG_NAMED(SELOG, "Compare new cost %d vs old cost %d", new_cost, succ_state->g);
        if (new_cost < succ_state->g) {
            succ_state->g = new_cost;
            //for forward search
            succ_state->bestpredstate = s;
            storeParent(succ_state,s,new_cost,expansion_step);
            if (succ_state->iteration_closed != m_iteration) {
                succ_state->f = computeKey(succ_state);
                if (m_open.contains(succ_state)) {
                    m_open.decrease(succ_state);
                } else {
                    //Set C of current successor
                    succ_state->C = expansion_step;
                    seen_states.push_back(succ_state);
                    m_open.push(succ_state);
                }
            } else if (!succ_state->incons) {
                m_incons.push_back(succ_state);
            }
        }
    }
    //update E of current state
    s->E = expansion_step;
    expansion_step++;
}

// Recompute heuristics for all states.
void TRAPlanner::recomputeHeuristics()
{
    for (TRAState* s : m_states) {
        s->h = m_heur->GetGoalHeuristic(s->state_id);
    }
}

// Recompute the f-values of all states in OPEN and reorder OPEN.
void TRAPlanner::reorderOpen()
{
    for (auto it = m_open.begin(); it != m_open.end(); ++it) {
        (*it)->f = computeKey(*it);
    }
    m_open.make();
}


int TRAPlanner::computeKey(TRAState* s) const
{
    return s->g + (unsigned int)(m_curr_eps * s->h);
}

// Get the search state corresponding to a graph state, creating a new state if
// one has not been created yet.
TRAState* TRAPlanner::getSearchState(int state_id)
{
    if (m_graph_to_search_map.size() <= state_id) {
        m_graph_to_search_map.resize(state_id + 1, -1);
    }

    if (m_graph_to_search_map[state_id] == -1) {
        return createState(state_id);
    } else {
        return m_states[m_graph_to_search_map[state_id]];
    }
}

// Create a new search state for a graph state.
TRAState* TRAPlanner::createState(int state_id)
{
    assert(state_id < m_graph_to_search_map.size());

    m_graph_to_search_map[state_id] = (int)m_states.size();

    TRAState* ss = new TRAState;
    ss->state_id = state_id;
    ss->call_number = 0;
    m_states.push_back(ss);

    return ss;
}

// Lazily (re)initialize a search state.
void TRAPlanner::reinitSearchState(TRAState* state)
{
    if (state->call_number != m_call_number) {
        ROS_DEBUG_NAMED(SELOG, "Reinitialize state %d", state->state_id);
        state->g = INFINITECOST;
        state->h = m_heur->GetGoalHeuristic(state->state_id);
        state->f = INFINITECOST;
        state->eg = INFINITECOST;
        state->iteration_closed = 0;
        state->call_number = m_call_number;
        state->bestpredstate = nullptr;
        state->bestnextstate = nullptr;
        state->incons = false;
    }
}

// Extract the path from the start state up to a new state.
void TRAPlanner::extractPath(TRAState* to_state, std::vector<int>& solution, int& cost) const
{
    //forward search
    for (TRAState* s = to_state; s; s = s->bestpredstate) {
        solution.push_back(s->state_id);
    }
    std::reverse(solution.begin(), solution.end());
    cost = to_state->g;
}

bool TRAPlanner::RestoreSearchTree(unsigned int expansionStep)
{
    m_open.clear();
    //need to clear closed
    std::vector<TRAState*> current_seen;

    TRAState* parent;
    unsigned int parentGVal;

    if(expansionStep<=0)
        InitializeSearch();
    else
    {
        for(int i=0;i<seen_states.size();i++)
        {
            TRAState* current = seen_states[i];
            //state created and expanded
            if(current->E <= expansionStep)
            {
                updateParents(current,expansionStep,current->bestpredstate,&parentGVal);
                current->g = parentGVal;
                //insert in closed
                current_seen.push_back(current);
            }
            //state created only
            else if(current->C <= expansionStep)
            {
                updateParents(current,expansionStep,current->bestpredstate,&parentGVal);
                current->g = parentGVal;
                current->v = INFINITECOST;
                computeKey(current);
                m_open.push(current);
                current->E = INFINITECOST;
                current_seen.push_back(current);
            }
            //state not created yet
            else
            {
                current->v = INFINITECOST;
                current->g = INFINITECOST;
                current->C = INFINITECOST;
                current->E = INFINITECOST;
                current->bestpredstate = nullptr;
                current->parent_hist.clear();
                current->gval_hist.clear();
            }
        }
        seen_states = current_seen;
        expansion_step = expansionStep+1;
    }
}

bool TRAPlanner::updateParents(TRAState* state, unsigned int expansionStep, TRAState* latestParent, unsigned int *latestGVal)
{
    unsigned int latestParentStep = 0;
    latestParent = nullptr;
    *latestGVal = 0;

    for(int i=0;i<state->parent_hist.size();i++)
    {
        TRAState* parent = state->parent_hist[i];
        //this parent is a valid one for the given expansion step
        if(parent->E <= expansionStep)
        {
            if(parent->E > latestParentStep)
            {
                latestParent = parent;
                latestGVal = &state->gval_hist[i];
                latestParentStep = parent->E;
            }
        }
        //this parent is not valid for the given expansion step
        else
        {
            state->parent_hist.erase(state->parent_hist.begin() + i);
            state->gval_hist.erase(state->gval_hist.begin() + i);          
        }
    }   
}

bool TRAPlanner::storeParent(TRAState* succ_state, TRAState* state, unsigned int gVal, unsigned int expansionStep)
{
    state->parent_hist.push_back(succ_state);
    state->gval_hist.push_back(gVal);
    return 1;
}

void TRAPlanner::heuristicChanged()
{   
    bool done = false;
    std::vector<unsigned int> inconsE;

    while(!done)
    {
        TRAState* minState = m_open.min();
        //loop on closed
        for (TRAState* s : m_states) 
        {
            unsigned int cost = s->v + (unsigned int)(m_curr_eps * s->h);
            if(cost > minState->f && minState->C < s->E)
                inconsE.push_back(s->E);
        }
        if(inconsE.empty())
            done = true;
        else
        {
            unsigned int newStep  = (*std::min_element(inconsE.begin(),inconsE.end())) - 1;
            RestoreSearchTree(newStep); 
        }
    }

}

void TRAPlanner::InitializeSearch()
{
    //clear closed
    TRAState* start = getSearchState(m_start_state_id);
    m_open.push(start);
    start->g = 0;
    computeKey(start);
    expansion_step = 1;
    start->C = 0;
    seen_states.push_back(start);
    start->E = INFINITECOST;
}


void TRAPlanner::Recomputegval(TRAState* state)
{

}


// used for backward search
void TRAPlanner::UpdatePreds(TRAState* state)
{
    /*std::vector<int> PredIDV;
    std::vector<int> CostV;
    CKey key;
    TRAState* predstate;

    m_space->GetPreds(state->MDPstate->StateID, &PredIDV, &CostV);

    // iterate through predecessors of s
    for (int pind = 0; pind < (int)PredIDV.size(); pind++) {
        CMDPSTATE* PredMDPState = GetState(PredIDV[pind], pSearchStateSpace);
        predstate = (TRAState*)(PredMDPState->PlannerSpecificData);
        if (predstate->callnumberaccessed != pSearchStateSpace->callnumber) {
            ReInitializeSearchStateInfo(predstate, pSearchStateSpace);
        }

        // see if we can improve the value of predstate
        if (predstate->g > state->v + CostV[pind]) {
            predstate->g = state->v + CostV[pind];
            predstate->bestnextstate = state->MDPstate;
            predstate->costtobestnextstate = CostV[pind];

            // re-insert into heap if not closed yet
            if (predstate->iterationclosed != pSearchStateSpace->searchiteration) {
                key.key[0] = predstate->g + (int)(pSearchStateSpace->eps*predstate->h);
//                key.key[1] = predstate->h;
                if (predstate->heapindex != 0) {
                    pSearchStateSpace->heap->updateheap(predstate,key);
                }
                else {
                    pSearchStateSpace->heap->insertheap(predstate,key);
                }
            }
            else if (predstate->listelem[ARA_AD_INCONS_LIST_ID] == NULL) {
                // take care of incons list
                pSearchStateSpace->inconslist->insert(predstate, ARA_AD_INCONS_LIST_ID);
            }
        }
    } // for predecessors*/
}

// used for forward search
void TRAPlanner::UpdateSuccs(TRAState* state)
{
    /*std::vector<int> SuccIDV;
    std::vector<int> CostV;
    CKey key;
    TRAState* succstate;

    m_space->GetSuccs(state->MDPstate->StateID, &SuccIDV, &CostV);

    // iterate through predecessors of s
    for (int sind = 0; sind < (int)SuccIDV.size(); sind++) {
        CMDPSTATE* SuccMDPState = GetState(SuccIDV[sind], pSearchStateSpace);
        int cost = CostV[sind];

        succstate = (TRAState*)(SuccMDPState->PlannerSpecificData);
        if (succstate->callnumberaccessed != pSearchStateSpace->callnumber) {
            ReInitializeSearchStateInfo(succstate, pSearchStateSpace);
        }

        // update generated index

        // see if we can improve the value of succstate
        // taking into account the cost of action
        if (succstate->g > state->v + cost) {
            succstate->g = state->v + cost;
            succstate->bestpredstate = state->MDPstate;

            // re-insert into heap if not closed yet
            if (succstate->iterationclosed != pSearchStateSpace->searchiteration) {
                key.key[0] = succstate->g + (int)(pSearchStateSpace->eps*succstate->h);
//                key.key[1] = succstate->h;

                if (succstate->heapindex != 0) {
                    pSearchStateSpace->heap->updateheap(succstate,key);
                }
                else {
                    pSearchStateSpace->heap->insertheap(succstate,key);
                }
            }
            else if (succstate->listelem[ARA_AD_INCONS_LIST_ID] == NULL) {
                // take care of incons list
                pSearchStateSpace->inconslist->insert(succstate, ARA_AD_INCONS_LIST_ID);
            }
        } // check for cost improvement

    } // for actions
}

void TRAPlanner::BuildNewOPENList()
{
    /*TRAState* state;
    CKey key;
    CHeap* pheap = pSearchStateSpace->heap;
    CList* pinconslist = pSearchStateSpace->inconslist;

    // move incons into open
    while (pinconslist->firstelement != NULL) {
        state = (TRAState*)pinconslist->firstelement->liststate;

        // compute f-value
        key.key[0] = state->g + (int)(pSearchStateSpace->eps * state->h);
//        key.key[1] = state->h;

        // insert into OPEN
        pheap->insertheap(state, key);
        // remove from INCONS
        pinconslist->remove(state, ARA_AD_INCONS_LIST_ID);
    }*/
}


void TRAPlanner::PrintSearchState(TRAState* state, FILE* fOut)
{
    SBPL_FPRINTF(fOut, "state %d: h=%d g=%u v=%u iterc=%d callnuma=%d heapind=%d inconslist=%d\n", state->MDPstate->StateID, state->h, state->g, state->v, state->iterationclosed, state->callnumberaccessed, state->heapindex, state->listelem[ARA_AD_INCONS_LIST_ID] ? 1 : 0);
    m_space->PrintState(state->state_id, true, fOut);
}


namespace motion {

SBPLPlannerPtr TRAPlannerAllocator::allocate(
    const RobotPlanningSpacePtr& pspace,
    const RobotHeuristicPtr& heuristic)
{
    const bool forward_search = true;
    auto search = std::make_shared<TRAPlanner>(pspace.get(), heuristic.get(),forward_search);

    double epsilon;
    pspace->params()->param("epsilon", epsilon, 1.0);
    search->set_initialsolution_eps(epsilon);

    bool search_mode;
    pspace->params()->param("search_mode", search_mode, false);
    search->set_search_mode(search_mode);

    double repair_time;
    if (pspace->params()->getParam("repair_time", repair_time)) {
        search->setAllowedRepairTime(repair_time);
    }

    return search;
}

} // namespace motion

} // namespace sbpl

