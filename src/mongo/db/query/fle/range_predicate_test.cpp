/**
 *    Copyright (C) 2022-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/crypto/fle_crypto.h"
#include "mongo/db/matcher/expression_expr.h"
#include "mongo/db/matcher/expression_leaf.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/query/fle/encrypted_predicate.h"
#include "mongo/db/query/fle/encrypted_predicate_test_fixtures.h"
#include "mongo/db/query/fle/range_predicate.h"
#include "mongo/idl/server_parameter_test_util.h"
#include "mongo/unittest/unittest.h"

namespace mongo::fle {
namespace {
class MockRangePredicate : public RangePredicate {
public:
    MockRangePredicate(const QueryRewriterInterface* rewriter) : RangePredicate(rewriter) {}

    MockRangePredicate(const QueryRewriterInterface* rewriter,
                       TagMap tags,
                       std::set<StringData> encryptedFields)
        : RangePredicate(rewriter), _tags(tags), _encryptedFields(encryptedFields) {}

    void setEncryptedTags(std::pair<StringData, int> fieldvalue, std::vector<PrfBlock> tags) {
        _encryptedFields.insert(fieldvalue.first);
        _tags[fieldvalue] = tags;
    }


    bool payloadValid = true;

protected:
    bool isPayload(const BSONElement& elt) const override {
        return payloadValid;
    }

    bool isPayload(const Value& v) const override {
        return payloadValid;
    }

    std::vector<PrfBlock> generateTags(BSONValue payload) const {
        return stdx::visit(
            OverloadedVisitor{[&](BSONElement p) {
                                  auto parsedPayload = p.Obj().firstElement();
                                  auto fieldName = parsedPayload.fieldNameStringData();

                                  std::vector<BSONElement> range;
                                  auto payloadAsArray = parsedPayload.Array();
                                  for (auto&& elt : payloadAsArray) {
                                      range.push_back(elt);
                                  }

                                  std::vector<PrfBlock> allTags;
                                  for (auto i = range[0].Number(); i <= range[1].Number(); i++) {
                                      ASSERT(_tags.find({fieldName, i}) != _tags.end());
                                      auto temp = _tags.find({fieldName, i})->second;
                                      for (auto tag : temp) {
                                          allTags.push_back(tag);
                                      }
                                  }
                                  return allTags;
                              },
                              [&](std::reference_wrapper<Value> v) {
                                  if (v.get().isArray()) {
                                      auto arr = v.get().getArray();
                                      std::vector<PrfBlock> allTags;
                                      for (auto& val : arr) {
                                          allTags.push_back(PrfBlock(
                                              {static_cast<unsigned char>(val.coerceToInt())}));
                                      }
                                      return allTags;
                                  } else {
                                      return std::vector<PrfBlock>{};
                                  }
                              }},
            payload);
    }

private:
    TagMap _tags;
    std::set<StringData> _encryptedFields;
};
class RangePredicateRewriteTest : public EncryptedPredicateRewriteTest {
public:
    RangePredicateRewriteTest() : _predicate(&_mock) {}

protected:
    MockRangePredicate _predicate;
};

TEST_F(RangePredicateRewriteTest, MatchRangeRewrite) {
    RAIIServerParameterControllerForTest controller("featureFlagFLE2Range", true);

    int start = 1;
    int end = 3;
    StringData encField = "ssn";

    std::vector<PrfBlock> tags1 = {{1}, {2}, {3}};
    std::vector<PrfBlock> tags2 = {{4}, {5}, {6}};
    std::vector<PrfBlock> tags3 = {{7}, {8}, {9}};

    _predicate.setEncryptedTags({encField, 1}, tags1);
    _predicate.setEncryptedTags({encField, 2}, tags2);
    _predicate.setEncryptedTags({encField, 3}, tags3);

    std::vector<PrfBlock> allTags = {{1}, {2}, {3}, {4}, {5}, {6}, {7}, {8}, {9}};

    // The field redundancy is so that we can pull out the field
    // name in the mock version of rewriteRangePayloadAsTags.
    BSONObj query =
        BSON(encField << BSON("$between" << BSON(encField << BSON_ARRAY(start << end))));

    auto inputExpr = BetweenMatchExpression(encField, query[encField]["$between"], nullptr);

    assertRewriteToTags(_predicate, &inputExpr, toBSONArray(std::move(allTags)));
}

TEST_F(RangePredicateRewriteTest, AggRangeRewrite) {
    auto input = fromjson(R"({$between: ["$age", {$literal: [1, 2, 3]}]})");
    auto inputExpr =
        ExpressionBetween::parseExpression(&_expCtx, input, _expCtx.variablesParseState);

    auto expected = makeTagDisjunction(&_expCtx, toValues({{1}, {2}, {3}}));

    auto actual = _predicate.rewrite(inputExpr.get());

    ASSERT_BSONOBJ_EQ(actual->serialize(false).getDocument().toBson(),
                      expected->serialize(false).getDocument().toBson());
}

TEST_F(RangePredicateRewriteTest, AggRangeRewriteNoOp) {
    auto input = fromjson(R"({$between: ["$age", {$literal: [1, 2, 3]}]})");
    auto inputExpr =
        ExpressionBetween::parseExpression(&_expCtx, input, _expCtx.variablesParseState);

    auto expected = inputExpr;

    _predicate.payloadValid = false;
    auto actual = _predicate.rewrite(inputExpr.get());
    ASSERT(actual == nullptr);
}

BSONObj generateFFP(StringData path, int lb, int ub, int min, int max) {
    auto indexKey = getIndexKey();
    FLEIndexKeyAndId indexKeyAndId(indexKey.data, indexKeyId);
    auto userKey = getUserKey();
    FLEUserKeyAndId userKeyAndId(userKey.data, indexKeyId);

    auto edges = minCoverInt32(lb, true, ub, true, min, max, 1);
    auto ffp = FLEClientCrypto::serializeFindRangePayload(indexKeyAndId, userKeyAndId, edges, 0);

    BSONObjBuilder builder;
    toEncryptedBinData(path, EncryptedBinDataType::kFLE2FindRangePayload, ffp, &builder);
    return builder.obj();
}

std::unique_ptr<MatchExpression> generateBetweenWithFFP(StringData path, int lb, int ub) {
    auto ffp = generateFFP(path, lb, ub, 0, 255);
    return std::make_unique<BetweenMatchExpression>(path, ffp.firstElement());
}

std::unique_ptr<Expression> generateBetweenWithFFP(ExpressionContext* expCtx,
                                                   StringData path,
                                                   int lb,
                                                   int ub) {
    auto ffp = Value(generateFFP(path, lb, ub, 0, 255).firstElement());
    auto ffpExpr = make_intrusive<ExpressionConstant>(expCtx, ffp);
    auto fieldpath = ExpressionFieldPath::createPathFromString(
        expCtx, path.toString(), expCtx->variablesParseState);
    std::vector<boost::intrusive_ptr<Expression>> children = {std::move(fieldpath),
                                                              std::move(ffpExpr)};
    return std::make_unique<ExpressionBetween>(expCtx, std::move(children));
}

TEST_F(RangePredicateRewriteTest, CollScanRewriteMatch) {
    _mock.setForceEncryptedCollScanForTest();
    auto input = generateBetweenWithFFP("age", 23, 35);
    auto result = _predicate.rewrite(input.get());
    ASSERT(result);
    ASSERT_EQ(result->matchType(), MatchExpression::EXPRESSION);
    auto* expr = static_cast<ExprMatchExpression*>(result.get());
    auto aggExpr = expr->getExpression();
    auto expected = fromjson(R"({
        "$_internalFleBetween": {
            "field": "$age",
            "edc": [
                {
                    "$binary": {
                        "base64": "CJb59SJCWcnn4u4uS1KHMphf8zK7M5+fUoFTzzUMqFVv",
                        "subType": "6"
                    }
                },
                {
                    "$binary": {
                        "base64": "CDE4/QorDvn6+GnmlPJtxQ5pZmwKOt/F48HmNrQuVJ1o",
                        "subType": "6"
                    }
                },
                {
                    "$binary": {
                        "base64": "CE0h7vfdciFBeqIk1N14ZXw/jzFT0bLfXcNyiPRsg4W4",
                        "subType": "6"
                    }
                }
                
            ],
            "counter": {$numberLong: "0"},
            "server": {
                "$binary": {
                    "base64": "COuac/eRLYakKX6B0vZ1r3QodOQFfjqJD+xlGiPu4/Ps",
                    "subType": "6"
                }
            }
        }
    })");
    ASSERT_BSONOBJ_EQ(aggExpr->serialize(false).getDocument().toBson(), expected);
}

TEST_F(RangePredicateRewriteTest, CollScanRewriteAgg) {
    _mock.setForceEncryptedCollScanForTest();
    auto input = generateBetweenWithFFP(&_expCtx, "age", 23, 35);
    auto result = _predicate.rewrite(input.get());
    ASSERT(result);
    auto expected = fromjson(R"({
        "$_internalFleBetween": {
            "field": "$age",
            "edc": [
                {
                    "$binary": {
                        "base64": "CJb59SJCWcnn4u4uS1KHMphf8zK7M5+fUoFTzzUMqFVv",
                        "subType": "6"
                    }
                },
                {
                    "$binary": {
                        "base64": "CDE4/QorDvn6+GnmlPJtxQ5pZmwKOt/F48HmNrQuVJ1o",
                        "subType": "6"
                    }
                },
                {
                    "$binary": {
                        "base64": "CE0h7vfdciFBeqIk1N14ZXw/jzFT0bLfXcNyiPRsg4W4",
                        "subType": "6"
                    }
                }
                
            ],
            "counter": {$numberLong: "0"},
            "server": {
                "$binary": {
                    "base64": "COuac/eRLYakKX6B0vZ1r3QodOQFfjqJD+xlGiPu4/Ps",
                    "subType": "6"
                }
            }
        }
    })");
    ASSERT_BSONOBJ_EQ(result->serialize(false).getDocument().toBson(), expected);
}

};  // namespace
}  // namespace mongo::fle