﻿/* Copyright (c) 2012-2017 The ANTLR Project. All rights reserved.
 * Use of this file is governed by the BSD 3-clause license that
 * can be found in the LICENSE.txt file in the project root.
 */

#include "atn/EmptyPredictionContext.h"
#include "misc/MurmurHash.h"
#include "atn/ArrayPredictionContext.h"
#include "RuleContext.h"
#include "ParserRuleContext.h"
#include "atn/RuleTransition.h"
#include "support/Arrays.h"
#include "support/CPPUtils.h"

#include "atn/PredictionContext.h"

using namespace antlr4;
using namespace antlr4::misc;
using namespace antlr4::atn;

using namespace antlrcpp;

size_t PredictionContext::globalNodeCount = 0;
const Ref<PredictionContext> PredictionContext::EMPTY = std::make_shared<EmptyPredictionContext>();

//----------------- PredictionContext ----------------------------------------------------------------------------------

PredictionContext::PredictionContext(size_t cachedHashCode) : id(globalNodeCount++), cachedHashCode(cachedHashCode)  {
}

PredictionContext::~PredictionContext() {
}

Ref<PredictionContext> PredictionContext::fromRuleContext(const ATN &atn, RuleContext *outerContext) {
  if (outerContext == nullptr) {
    return PredictionContext::EMPTY;
  }

  // if we are in RuleContext of start rule, s, then PredictionContext
  // is EMPTY. Nobody called us. (if we are empty, return empty)
  if (outerContext->parent == nullptr || outerContext == &ParserRuleContext::EMPTY) {
    return PredictionContext::EMPTY;
  }

  // If we have a parent, convert it to a PredictionContext graph
  Ref<PredictionContext> parent = PredictionContext::fromRuleContext(atn, dynamic_cast<RuleContext *>(outerContext->parent));

  ATNState *state = atn.states.at(outerContext->invokingState);
  RuleTransition *transition = (RuleTransition *)state->transitions[0];
  return SingletonPredictionContext::create(parent, transition->followState->stateNumber);
}

bool PredictionContext::isEmpty() const {
  return this == EMPTY.get();
}

bool PredictionContext::hasEmptyPath() const {
  // since EMPTY_RETURN_STATE can only appear in the last position, we check last one
  return getReturnState(size() - 1) == EMPTY_RETURN_STATE;
}

size_t PredictionContext::hashCode() const {
  return cachedHashCode;
}

size_t PredictionContext::calculateEmptyHashCode() {
  size_t hash = MurmurHash::initialize(INITIAL_HASH);
  hash = MurmurHash::finish(hash, 0);
  return hash;
}

size_t PredictionContext::calculateHashCode(Ref<PredictionContext> parent, size_t returnState) {
  size_t hash = MurmurHash::initialize(INITIAL_HASH);
  hash = MurmurHash::update(hash, parent);
  hash = MurmurHash::update(hash, returnState);
  hash = MurmurHash::finish(hash, 2);
  return hash;
}

size_t PredictionContext::calculateHashCode(const std::vector<Ref<PredictionContext>> &parents,
                                            const std::vector<size_t> &returnStates) {
  size_t hash = MurmurHash::initialize(INITIAL_HASH);

  for (auto parent : parents) {
    hash = MurmurHash::update(hash, parent);
  }

  for (auto returnState : returnStates) {
    hash = MurmurHash::update(hash, returnState);
  }

  return MurmurHash::finish(hash, parents.size() + returnStates.size());
}

size_t PredictionContext::calculateHashCode(const std::vector<SingletonPredictionContext> &contexts) {
    size_t hash = MurmurHash::initialize(INITIAL_HASH);

    for (const SingletonPredictionContext& context : contexts) {
        hash = MurmurHash::update(hash, context.parent);
    }

    for (const SingletonPredictionContext& context : contexts) {
        hash = MurmurHash::update(hash, context.returnState);
    }

    return MurmurHash::finish(hash, 2 * contexts.size());
}

Ref<PredictionContext> PredictionContext::merge(const Ref<PredictionContext> &a,
  const Ref<PredictionContext> &b, bool rootIsWildcard, PredictionContextMergeCache *mergeCache) {
  assert(a && b);

  // share same graph if both same
  if (a == b || *a == *b) {
    return a;
  }

  if (is<SingletonPredictionContext>(a) && is<SingletonPredictionContext>(b)) {
    return mergeSingletons(std::dynamic_pointer_cast<SingletonPredictionContext>(a),
                           std::dynamic_pointer_cast<SingletonPredictionContext>(b), rootIsWildcard, mergeCache);
  }

  // At least one of a or b is array.
  // If one is $ and rootIsWildcard, return $ as * wildcard.
  if (rootIsWildcard) {
    if (is<EmptyPredictionContext>(a)) {
      return a;
    }
    if (is<EmptyPredictionContext>(b)) {
      return b;
    }
  }

  // convert singleton so both are arrays to normalize
  Ref<ArrayPredictionContext> left;
  if (is<SingletonPredictionContext>(a)) {
    left = std::make_shared<ArrayPredictionContext>(std::dynamic_pointer_cast<SingletonPredictionContext>(a));
  } else {
    left = std::dynamic_pointer_cast<ArrayPredictionContext>(a);
  }
  Ref<ArrayPredictionContext> right;
  if (is<SingletonPredictionContext>(b)) {
    right = std::make_shared<ArrayPredictionContext>(std::dynamic_pointer_cast<SingletonPredictionContext>(b));
  } else {
    right = std::dynamic_pointer_cast<ArrayPredictionContext>(b);
  }
  return mergeArrays(left, right, rootIsWildcard, mergeCache);
}

Ref<PredictionContext> PredictionContext::mergeSingletons(const Ref<SingletonPredictionContext> &a,
  const Ref<SingletonPredictionContext> &b, bool rootIsWildcard, PredictionContextMergeCache *mergeCache) {

  if (mergeCache != nullptr) { // Can be null if not given to the ATNState from which this call originates.
    auto existing = mergeCache->get(a, b);
    if (existing) {
      return existing;
    }
    existing = mergeCache->get(b, a);
    if (existing) {
      return existing;
    }
  }

  Ref<PredictionContext> rootMerge = mergeRoot(a, b, rootIsWildcard);
  if (rootMerge) {
    if (mergeCache != nullptr) {
      mergeCache->put(a, b, rootMerge);
    }
    return rootMerge;
  }

  Ref<PredictionContext> parentA = a->parent;
  Ref<PredictionContext> parentB = b->parent;
  if (a->returnState == b->returnState) { // a == b
    Ref<PredictionContext> parent = merge(parentA, parentB, rootIsWildcard, mergeCache);

    // If parent is same as existing a or b parent or reduced to a parent, return it.
    if (parent == parentA) { // ax + bx = ax, if a=b
      return a;
    }
    if (parent == parentB) { // ax + bx = bx, if a=b
      return b;
    }

    // else: ax + ay = a'[x,y]
    // merge parents x and y, giving array node with x,y then remainders
    // of those graphs.  dup a, a' points at merged array
    // new joined parent so create new singleton pointing to it, a'
    Ref<PredictionContext> a_ = SingletonPredictionContext::create(parent, a->returnState);
    if (mergeCache != nullptr) {
      mergeCache->put(a, b, a_);
    }
    return a_;
  } else {
    // a != b payloads differ
    // see if we can collapse parents due to $+x parents if local ctx
    Ref<PredictionContext> a_;
    if (a->returnState > b->returnState) {
        std::vector<SingletonPredictionContext> contexts = { *b, *a };
        a_ = std::make_shared<ArrayPredictionContext>(contexts);
    } else {
        std::vector<SingletonPredictionContext> contexts = { *a, *b };
        a_ = std::make_shared<ArrayPredictionContext>(contexts);
    }
    if (mergeCache != nullptr) {
      mergeCache->put(a, b, a_);
    }
    return a_;
  }
}

Ref<PredictionContext> PredictionContext::mergeRoot(const Ref<SingletonPredictionContext> &a,
  const Ref<SingletonPredictionContext> &b, bool rootIsWildcard) {
  if (rootIsWildcard) {
    if (a == EMPTY) { // * + b = *
      return EMPTY;
    }
    if (b == EMPTY) { // a + * = *
      return EMPTY;
    }
  } else {
    if (a == EMPTY && b == EMPTY) { // $ + $ = $
      return EMPTY;
    }
    if (a == EMPTY) { // $ + x = [$,x]
      std::vector<SingletonPredictionContext> contexts = {*b, SingletonPredictionContext(nullptr, EMPTY_RETURN_STATE)};
      Ref<PredictionContext> joined = std::make_shared<ArrayPredictionContext>(contexts);
      return joined;
    }
    if (b == EMPTY) { // x + $ = [$,x] ($ is always first if present)
      std::vector<SingletonPredictionContext> contexts = {*a, SingletonPredictionContext(nullptr, EMPTY_RETURN_STATE)};
      Ref<PredictionContext> joined = std::make_shared<ArrayPredictionContext>(contexts);
      return joined;
    }
  }
  return nullptr;
}

Ref<PredictionContext> PredictionContext::mergeArrays(const Ref<ArrayPredictionContext> &a,
  const Ref<ArrayPredictionContext> &b, bool rootIsWildcard, PredictionContextMergeCache *mergeCache) {

  if (mergeCache != nullptr) {
    auto existing = mergeCache->get(a, b);
    if (existing) {
      return existing;
    }
    existing = mergeCache->get(b, a);
    if (existing) {
      return existing;
    }
  }

  // merge sorted payloads a + b => M
  std::vector<SingletonPredictionContext> contexts;
  contexts.reserve(a->contexts.size() + b->contexts.size());
  std::vector<SingletonPredictionContext>::const_iterator i = a->contexts.begin(), i_end = a->contexts.end();
  std::vector<SingletonPredictionContext>::const_iterator j = b->contexts.begin(), j_end = b->contexts.end();
  while (i != i_end && j != j_end) {
      if (i->returnState == j->returnState) {
          if (i->parent && j->parent && (i->returnState == EMPTY_RETURN_STATE || *i->parent == *j->parent)) {
              contexts.emplace_back(i->parent, i->returnState);
          } else {
              contexts.emplace_back(merge(i->parent, j->parent, rootIsWildcard, mergeCache),
                                    i->returnState);
          }
          ++i;
          ++j;
      } else if (i->returnState < j->returnState) {
          contexts.emplace_back(i->parent, i->returnState);
          ++i;
      } else {
          contexts.emplace_back(j->parent, j->returnState);
          ++j;
      }
  }
  while (i != i_end) {
      contexts.emplace_back(i->parent, i->returnState);
      ++i;
  }
  while (j != j_end) {
      contexts.emplace_back(j->parent, j->returnState);
      ++j;
  }

  Ref<ArrayPredictionContext> M = std::make_shared<ArrayPredictionContext>(contexts);

  // if we created same array as a or b, return that instead
  // TO_DO: track whether this is possible above during merge sort for speed
  if (*M == *a) {
    if (mergeCache != nullptr) {
      mergeCache->put(a, b, a);
    }
    return a;
  }
  if (*M == *b) {
    if (mergeCache != nullptr) {
      mergeCache->put(a, b, b);
    }
    return b;
  }

  if (mergeCache != nullptr) {
    mergeCache->put(a, b, M);
  }
  return M;
}

bool PredictionContext::combineCommonParents(std::vector<Ref<PredictionContext>> &parents) {

  std::set<Ref<PredictionContext>> uniqueParents;
  for (size_t p = 0; p < parents.size(); ++p) {
    Ref<PredictionContext> parent = parents[p];
    if (uniqueParents.find(parent) == uniqueParents.end()) { // don't replace
      uniqueParents.insert(parent);
    }
  }

  for (size_t p = 0; p < parents.size(); ++p) {
    parents[p] = *uniqueParents.find(parents[p]);
  }

  return true;
}

std::string PredictionContext::toDOTString(const Ref<PredictionContext> &context) {
  if (context == nullptr) {
    return "";
  }

  std::stringstream ss;
  ss << "digraph G {\n" << "rankdir=LR;\n";

  std::vector<Ref<PredictionContext>> nodes = getAllContextNodes(context);
  std::sort(nodes.begin(), nodes.end(), [](const Ref<PredictionContext> &o1, const Ref<PredictionContext> &o2) {
    return o1->id - o2->id;
  });

  for (auto current : nodes) {
    if (is<SingletonPredictionContext>(current)) {
      std::string s = std::to_string(current->id);
      ss << "  s" << s;
      std::string returnState = std::to_string(current->getReturnState(0));
      if (is<EmptyPredictionContext>(current)) {
        returnState = "$";
      }
      ss << " [label=\"" << returnState << "\"];\n";
      continue;
    }
    Ref<ArrayPredictionContext> arr = std::static_pointer_cast<ArrayPredictionContext>(current);
    ss << "  s" << arr->id << " [shape=box, label=\"" << "[";
    bool first = true;
    for (const auto& ctx : arr->contexts) {
      if (!first) {
       ss << ", ";
      }
      if (ctx.returnState == EMPTY_RETURN_STATE) {
        ss << "$";
      } else {
        ss << ctx.returnState;
      }
      first = false;
    }
    ss << "]";
    ss << "\"];\n";
  }

  for (auto current : nodes) {
    if (current == EMPTY) {
      continue;
    }
    for (size_t i = 0; i < current->size(); i++) {
      if (!current->getParent(i)) {
        continue;
      }
      ss << "  s" << current->id << "->" << "s" << current->getParent(i)->id;
      if (current->size() > 1) {
        ss << " [label=\"parent[" << i << "]\"];\n";
      } else {
        ss << ";\n";
      }
    }
  }

  ss << "}\n";
  return ss.str();
}

// The "visited" map is just a temporary structure to control the retrieval process (which is recursive).
Ref<PredictionContext> PredictionContext::getCachedContext(const Ref<PredictionContext> &context,
  PredictionContextCache &contextCache, std::map<Ref<PredictionContext>, Ref<PredictionContext>> &visited) {
  if (context->isEmpty()) {
    return context;
  }

  {
    auto iterator = visited.find(context);
    if (iterator != visited.end())
      return iterator->second; // Not necessarly the same as context.
  }

  auto iterator = contextCache.find(context);
  if (iterator != contextCache.end()) {
    visited[context] = *iterator;

    return *iterator;
  }

  bool changed = false;

  std::vector<Ref<PredictionContext>> parents(context->size());
  for (size_t i = 0; i < parents.size(); i++) {
    Ref<PredictionContext> parent = getCachedContext(context->getParent(i), contextCache, visited);
    if (changed || parent != context->getParent(i)) {
      if (!changed) {
        parents.clear();
        for (size_t j = 0; j < context->size(); j++) {
          parents.push_back(context->getParent(j));
        }

        changed = true;
      }

      parents[i] = parent;
    }
  }

  if (!changed) {
    contextCache.insert(context);
    visited[context] = context;

    return context;
  }

  Ref<PredictionContext> updated;
  if (parents.empty()) {
    updated = EMPTY;
  } else if (parents.size() == 1) {
    updated = SingletonPredictionContext::create(parents[0], context->getReturnState(0));
    contextCache.insert(updated);
  } else {
    std::vector<SingletonPredictionContext> contexts;
    for (size_t i = 0; i < parents.size(); ++i) {
        contexts.push_back(SingletonPredictionContext(parents[i], std::dynamic_pointer_cast<ArrayPredictionContext>(context)->contexts[i].returnState));
    }
    updated = std::make_shared<ArrayPredictionContext>(contexts);
    contextCache.insert(updated);
  }

  visited[updated] = updated;
  visited[context] = updated;

  return updated;
}

std::vector<Ref<PredictionContext>> PredictionContext::getAllContextNodes(const Ref<PredictionContext> &context) {
  std::vector<Ref<PredictionContext>> nodes;
  std::set<PredictionContext *> visited;
  getAllContextNodes_(context, nodes, visited);
  return nodes;
}


void PredictionContext::getAllContextNodes_(const Ref<PredictionContext> &context, std::vector<Ref<PredictionContext>> &nodes,
  std::set<PredictionContext *> &visited) {

  if (visited.find(context.get()) != visited.end()) {
    return; // Already done.
  }

  visited.insert(context.get());
  nodes.push_back(context);

  for (size_t i = 0; i < context->size(); i++) {
    getAllContextNodes_(context->getParent(i), nodes, visited);
  }
}

std::string PredictionContext::toString() const {

  return antlrcpp::toString(this);
}

std::string PredictionContext::toString(Recognizer * /*recog*/) const {
  return toString();
}

std::vector<std::string> PredictionContext::toStrings(Recognizer *recognizer, int currentState) {
  return toStrings(recognizer, EMPTY, currentState);
}

std::vector<std::string> PredictionContext::toStrings(Recognizer *recognizer, const Ref<PredictionContext> &stop, int currentState) {

  std::vector<std::string> result;

  for (size_t perm = 0; ; perm++) {
    size_t offset = 0;
    bool last = true;
    PredictionContext *p = this;
    size_t stateNumber = currentState;

    std::stringstream ss;
    ss << "[";
    bool outerContinue = false;
    while (!p->isEmpty() && p != stop.get()) {
      size_t index = 0;
      if (p->size() > 0) {
        size_t bits = 1;
        while ((1ULL << bits) < p->size()) {
          bits++;
        }

        size_t mask = (1 << bits) - 1;
        index = (perm >> offset) & mask;
        last &= index >= p->size() - 1;
        if (index >= p->size()) {
          outerContinue = true;
          break;
        }
        offset += bits;
      }

      if (recognizer != nullptr) {
        if (ss.tellp() > 1) {
          // first char is '[', if more than that this isn't the first rule
          ss << ' ';
        }

        const ATN &atn = recognizer->getATN();
        ATNState *s = atn.states[stateNumber];
        std::string ruleName = recognizer->getRuleNames()[s->ruleIndex];
        ss << ruleName;
      } else if (p->getReturnState(index) != EMPTY_RETURN_STATE) {
        if (!p->isEmpty()) {
          if (ss.tellp() > 1) {
            // first char is '[', if more than that this isn't the first rule
            ss << ' ';
          }

          ss << p->getReturnState(index);
        }
      }
      stateNumber = p->getReturnState(index);
      p = p->getParent(index).get();
    }

    if (outerContinue)
      continue;

    ss << "]";
    result.push_back(ss.str());

    if (last) {
      break;
    }
  }

  return result;
}

//----------------- PredictionContextMergeCache ------------------------------------------------------------------------

Ref<PredictionContext> PredictionContextMergeCache::put(Ref<PredictionContext> const& key1, Ref<PredictionContext> const& key2,
                                                        Ref<PredictionContext> const& value) {
  Ref<PredictionContext> previous;

  auto iterator = _data.find(key1);
  if (iterator == _data.end())
    _data[key1][key2] = value;
  else {
    auto iterator2 = iterator->second.find(key2);
    if (iterator2 != iterator->second.end())
      previous = iterator2->second;
    iterator->second[key2] = value;
  }

  return previous;
}

Ref<PredictionContext> PredictionContextMergeCache::get(Ref<PredictionContext> const& key1, Ref<PredictionContext> const& key2) {
  auto iterator = _data.find(key1);
  if (iterator == _data.end())
    return nullptr;

  auto iterator2 = iterator->second.find(key2);
  if (iterator2 == iterator->second.end())
    return nullptr;

  return iterator2->second;
}

void PredictionContextMergeCache::clear() {
  _data.clear();
}

std::string PredictionContextMergeCache::toString() const {
  std::string result;
  for (auto pair : _data)
    for (auto pair2 : pair.second)
      result += pair2.second->toString() + "\n";

  return result;
}

size_t PredictionContextMergeCache::count() const {
  size_t result = 0;
  for (auto entry : _data)
    result += entry.second.size();
  return result;
}

