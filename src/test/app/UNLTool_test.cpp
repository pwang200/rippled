//------------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/ripple/rippled
    Copyright 2015 Ripple Labs Inc.

    Permission to use, copy, modify, and/or distribute this software for any
    purpose  with  or without fee is hereby granted, provided that the above
    copyright notice and this permission notice appear in all copies.

    THE  SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
    WITH  REGARD  TO  THIS  SOFTWARE  INCLUDING  ALL  IMPLIED  WARRANTIES  OF
    MERCHANTABILITY  AND  FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
    ANY  SPECIAL ,  DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
    WHATSOEVER  RESULTING  FROM  LOSS  OF USE, DATA OR PROFITS, WHETHER IN AN
    ACTION  OF  CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
    OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
*/
//==============================================================================

#include <ripple/app/misc/ValidatorList.h>
#include <ripple/basics/Slice.h>
#include <ripple/basics/base64.h>
#include <ripple/basics/strHex.h>
#include <ripple/protocol/HashPrefix.h>
#include <ripple/protocol/PublicKey.h>
#include <ripple/protocol/SecretKey.h>
#include <ripple/protocol/Sign.h>
#include <ripple/protocol/digest.h>
#include <test/jtx.h>

namespace ripple {
namespace test {

class UNLTool_test : public beast::unit_test::suite
{
private:
    struct Validator
    {
        SecretKey masterSecret;
        PublicKey masterPublic;
        SecretKey signingSecret;
        PublicKey signingPublic;
        std::string manifest;

        std::string tokenString() const
        {
            Json::Value jv;
            jv["validation_secret_key"] = strHex(signingSecret);
            jv["manifest"] = manifest;

            return ripple::base64_encode(to_string(jv));
        }

        void print(std::string const & first_line, std::string const & last_line) const
        {
            std::cout << first_line << std::endl;
            std::cout << "masterSecret Base58 " << toBase58(TokenType::NodePrivate, masterSecret) << std::endl;
            std::cout << "masterPublic Base58 " << toBase58(TokenType::NodePublic, masterPublic) << std::endl;
            std::cout << "signingSecret Base58 " << toBase58(TokenType::NodePrivate, signingSecret) << std::endl;
            std::cout << "signingPublic Base58 " << toBase58(TokenType::NodePublic, signingPublic) << std::endl;
            std::cout << "masterSecret hex " << strHex(masterSecret) << std::endl;
            std::cout << "masterPublic hex " << strHex(masterPublic) << std::endl;
            std::cout << "signingSecret hex " << strHex(signingSecret) << std::endl;
            std::cout << "signingPublic hex " << strHex(signingPublic) << std::endl;
            std::cout << "manifest " << manifest << std::endl;
            std::cout << "token " << tokenString() << std::endl;
            std::cout << last_line << std::endl;

        }
    };

    static std::string
    makeManifestString(
        PublicKey const& pk,
        SecretKey const& sk,
        PublicKey const& spk,
        SecretKey const& ssk,
        int seq)
    {
        STObject st(sfGeneric);
        st[sfSequence] = seq;
        st[sfPublicKey] = pk;

        if (seq != std::numeric_limits<std::uint32_t>::max())
        {
            st[sfSigningPubKey] = spk;
            sign(st, HashPrefix::manifest, *publicKeyType(spk), ssk);
        }

        sign(
            st,
            HashPrefix::manifest,
            *publicKeyType(pk),
            sk,
            sfMasterSignature);

        Serializer s;
        st.add(s);

        return std::string(static_cast<char const*>(s.data()), s.size());
    }

    std::string
    makeList(
        std::vector<Validator> const& validators,
        std::size_t sequence,
        std::size_t expiration)
    {
        std::string data = "{\"sequence\":" + std::to_string(sequence) +
            ",\"expiration\":" + std::to_string(expiration) +
            ",\"validators\":[";

        for (auto const& val : validators)
        {
            data += "{\"validation_public_key\":\"" + strHex(val.masterPublic) +
                "\",\"manifest\":\"" + val.manifest + "\"},";
        }

        data.pop_back();
        data += "]}";
        return base64_encode(data);
    }

    std::string
    signList(
        std::string const& blob,
        PublicKey const& pub, SecretKey const& sec)
    {
        auto const data = base64_decode(blob);
        return strHex(sign(pub, sec, makeSlice(data)));
    }

    static Validator
    createValidator(std::string masterSeed,
                    std::string signingSeed,
                    int seq)
    {
        Seed master_seed = generateSeed(masterSeed);
        Seed signing_seed = generateSeed(signingSeed);
        auto const masterSecret = generateSecretKey(KeyType::ed25519, master_seed);
        auto const masterPublic = derivePublicKey(KeyType::ed25519, masterSecret);
        auto const signingSecret = generateSecretKey(KeyType::secp256k1, signing_seed);
        auto const signingPublic = derivePublicKey(KeyType::secp256k1, signingSecret);

        return {masterSecret,
                masterPublic,
                signingSecret,
                signingPublic,
                base64_encode(makeManifestString(
                    masterPublic,
                    masterSecret,
                    signingPublic,
                    signingSecret,
                    seq))};
    }

    void
    testPrintUNL()
    {
        std::string const masterSeed = "";
        int num_validators = 10;
        int publisher_seq = 4;
        int unl_seq = 4;
        std::vector<int> validator_seq(num_validators, 4);

        std::string const siteUri = "testPrintUNL.test";
        ManifestCache manifests;
        jtx::Env env(*this);
        auto& app = env.app();
        auto timeKeeper = make_TimeKeeper(env.journal);
        auto trustedKeys = std::make_unique<ValidatorList>(
            manifests,
            manifests,
            env.app().timeKeeper(),
            app.config().legacy("database_path"),
            env.journal);

        auto const publisher = createValidator(
            masterSeed + std::string("_publisher"),
            std::string("_publisher_sign")+std::to_string(publisher_seq),
            publisher_seq);

        std::vector<std::string> cfgKeys1({strHex(publisher.masterPublic)});
        PublicKey emptyLocalKey;
        std::vector<std::string> emptyCfgKeys;
        BEAST_EXPECT(trustedKeys->load(emptyLocalKey, emptyCfgKeys, cfgKeys1));

        std::vector<Validator> validator_list;
        validator_list.reserve(num_validators);
        for(int i = 0; i < num_validators; ++i)
        {
            auto v = createValidator(
                masterSeed+std::to_string(i),
                masterSeed+std::string("_sign")+
                    std::to_string(i)+std::to_string(validator_seq[i]),
                validator_seq[i]);

            validator_list.push_back(v);
            auto ts = v.tokenString();
            auto token = loadValidatorToken({ts});
            BEAST_EXPECT(token);
            if(token)
            {
                BEAST_EXPECT(token->validationSecret == v.signingSecret);
                auto m = deserializeManifest(base64_decode(token->manifest));
                BEAST_EXPECT(m);
                if(m)
                {
                    BEAST_EXPECT(m->masterKey == v.masterPublic);
                    BEAST_EXPECT(m->signingKey == v.signingPublic);
                }
            }

            v.print(std::string("validator_")+std::to_string(i), "");
        }

        using namespace std::chrono_literals;
        NetClock::time_point const expiration = timeKeeper->now() + 24h * 365;
        auto const version = 1;
        auto const blob =
            makeList(
            validator_list, unl_seq, expiration.time_since_epoch().count());
        auto const sig1 = signList(blob, publisher.signingPublic, publisher.signingSecret);

        BEAST_EXPECT(
            ListDisposition::accepted ==
            trustedKeys->applyList(publisher.manifest, blob, sig1, version, siteUri)
                .disposition);

        publisher.print("publisher", "");
        std::cout << "UNL blob " << blob << std::endl;
        std::cout << "UNL sig " << sig1 << std::endl;
        std::cout << "masterSeed " << masterSeed << std::endl;
    }

public:
    void
    run() override
    {
        testPrintUNL();
    }
};

BEAST_DEFINE_TESTSUITE(UNLTool, app, ripple);

}  // namespace test
}  // namespace ripple
