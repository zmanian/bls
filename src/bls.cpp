/**
	@file
	@author MITSUNARI Shigeo(@herumi)
	@license modified new BSD license
	http://opensource.org/licenses/BSD-3-Clause
*/
#include <bls.hpp>
#include <mcl/bn.hpp>
#include <cybozu/crypto.hpp>
#include <cybozu/random_generator.hpp>
#include <vector>
#include <string>

typedef mcl::FpT<mcl::FpTag, 256> Fp;
typedef mcl::bn::BNT<Fp> BN;
typedef BN::Fp2 Fp2;
typedef BN::Fp6 Fp6;
typedef BN::Fp12 Fp12;
typedef BN::G1 G1;
typedef BN::G2 G2;

struct FrTag;
typedef mcl::FpT<FrTag, 256> Fr;
typedef std::vector<Fr> FrVec;

#define PUT(x) std::cout << #x << "=" << x << std::endl;

static cybozu::RandomGenerator& getRG()
{
	static cybozu::RandomGenerator rg;
	return rg;
}

namespace bls {

static const G2& getQ()
{
	static const G2 Q(
		Fp2("12723517038133731887338407189719511622662176727675373276651903807414909099441", "4168783608814932154536427934509895782246573715297911553964171371032945126671"),
		Fp2("13891744915211034074451795021214165905772212241412891944830863846330766296736", "7937318970632701341203597196594272556916396164729705624521405069090520231616")
	);
	return Q;
}

static void mapToG1(G1& P, const Fp& t)
{
	static mcl::bn::MapTo<Fp> mapTo;
	mapTo.calcG1(P, t);
}

static void HashAndMapToG1(G1& P, const std::string& m)
{
	std::string digest = cybozu::crypto::Hash::digest(cybozu::crypto::Hash::N_SHA256, m);
	Fp t;
	t.setArrayMask(digest.c_str(), digest.size());
	mapToG1(P, t);
}

template<class T, class G, class Vec>
void evalPoly(G& y, const T& x, const Vec& c)
{
	if (c.size() < 2) throw cybozu::Exception("bls:evalPoly:bad size") << c.size();
	y = c[c.size() - 1];
	for (int i = (int)c.size() - 2; i >= 0; i--) {
		G::mul(y, y, x);
		G::add(y, y, c[i]);
	}
}

template<class T, class G>
struct Wrap {
	const std::vector<T> *pv;
	Wrap(const std::vector<T>& v) : pv(&v) {}
	const G& operator[](size_t i) const
	{
		return (*pv)[i].getInner().get();
	}
	size_t size() const { return pv->size(); }
};

template<class T, class G>
struct WrapPointer {
	const T *const *v;
	size_t k;
	WrapPointer(const T *const *v, size_t k) : v(v), k(k) {}
	const G& operator[](size_t i) const
	{
		return v[i]->getInner().get();
	}
	size_t size() const { return k; }
};

struct Polynomial {
	FrVec c; // f[x] = sum_{i=0}^{k-1} c[i] x^i
	void init(const Fr& s, int k)
	{
		if (k < 2) throw cybozu::Exception("bls:Polynomial:init:bad k") << k;
		c.resize(k);
		c[0] = s;
		for (size_t i = 1; i < c.size(); i++) {
			c[i].setRand(getRG());
		}
	}
	// y = f(id)
	void eval(Fr& y, const Fr& id) const
	{
		if (id.isZero()) throw cybozu::Exception("bls:Polynomial:eval:id is zero");
		evalPoly(y, id, c);
	}
};

namespace impl {

struct Id {
	Fr v;
	const Fr& get() const { return v; }
};

struct SecretKey {
	Fr s;
	const Fr& get() const { return s; }
};

struct Sign {
	G1 sHm; // s Hash(m)
	const G1& get() const { return sHm; }
};

struct PublicKey {
	G2 sQ;
	const G2& get() const { return sQ; }
	void getStr(std::string& str) const
	{
		sQ.getStr(str, mcl::IoArrayRaw);
	}
};

} // mcl::bls::impl

/*
	recover f(0) by { (x, y) | x = S[i], y = f(x) = vec[i] }
*/
template<class G, class V1, class V2>
void LagrangeInterpolation(G& r, const V1& vec, const V2& S)
{
	/*
		delta_{i,S}(0) = prod_{j != i} S[j] / (S[j] - S[i]) = a / b
		where a = prod S[j], b = S[i] * prod_{j != i} (S[j] - S[i])
	*/
	const size_t k = S.size();
	if (vec.size() != k) throw cybozu::Exception("bls:LagrangeInterpolation:bad size") << vec.size() << k;
	if (k < 2) throw cybozu::Exception("bls:LagrangeInterpolation:too small size") << k;
	FrVec delta(k);
	Fr a = S[0];
	for (size_t i = 1; i < k; i++) {
		a *= S[i];
	}
	for (size_t i = 0; i < k; i++) {
		Fr b = S[i];
		for (size_t j = 0; j < k; j++) {
			if (j != i) {
				Fr v = S[j] - S[i];
				if (v.isZero()) throw cybozu::Exception("bls:LagrangeInterpolation:S has same id") << i << j;
				b *= v;
			}
		}
		delta[i] = a / b;
	}

	/*
		f(0) = sum_i f(S[i]) delta_{i,S}(0)
	*/
	r.clear();
	G t;
	for (size_t i = 0; i < delta.size(); i++) {
		G::mul(t, vec[i], delta[i]);
		r += t;
	}
}

template<class T>
std::ostream& writeAsHex(std::ostream& os, const T& t)
{
	std::string str;
	t.getStr(str, mcl::IoHexPrefix);
	return os << str;
}

void init()
{
	BN::init(mcl::bn::CurveFp254BNb);
	G1::setCompressedExpression();
	G2::setCompressedExpression();
	Fr::init(BN::param.r);
//	mcl::setIoMode(mcl::IoHeximal);
	assert(sizeof(Id) == sizeof(impl::Id));
	assert(sizeof(SecretKey) == sizeof(impl::SecretKey));
	assert(sizeof(PublicKey) == sizeof(impl::PublicKey));
	assert(sizeof(Sign) == sizeof(impl::Sign));
}

Id::Id(unsigned int id)
{
	getInner().v = id;
}

bool Id::operator==(const Id& rhs) const
{
	return getInner().v == rhs.getInner().v;
}

std::ostream& operator<<(std::ostream& os, const Id& id)
{
	return writeAsHex(os, id.getInner().v);
}

std::istream& operator>>(std::istream& is, Id& id)
{
	return is >> id.getInner().v;
}

bool Id::isZero() const
{
	return getInner().v.isZero();
}

void Id::set(const uint64_t *p)
{
	getInner().v.setArrayMask(p, keySize);
}

bool Sign::operator==(const Sign& rhs) const
{
	return getInner().sHm == rhs.getInner().sHm;
}

std::ostream& operator<<(std::ostream& os, const Sign& s)
{
	return writeAsHex(os, s.getInner().sHm);
}

std::istream& operator>>(std::istream& os, Sign& s)
{
	return os >> s.getInner().sHm;
}

bool Sign::verify(const PublicKey& pub, const std::string& m) const
{
	G1 Hm;
	HashAndMapToG1(Hm, m); // Hm = Hash(m)
	Fp12 e1, e2;
	BN::pairing(e1, getQ(), getInner().sHm); // e(Q, s Hm)
	BN::pairing(e2, pub.getInner().sQ, Hm); // e(sQ, Hm)
	return e1 == e2;
}

bool Sign::verify(const PublicKey& pub) const
{
	std::string str;
	pub.getInner().sQ.getStr(str);
	return verify(pub, str);
}

void Sign::recover(const SignVec& signVec, const IdVec& idVec)
{
	Wrap<Sign, G1> signW(signVec);
	Wrap<Id, Fr> idW(idVec);
	LagrangeInterpolation(getInner().sHm, signW, idW);
}

void Sign::recover(const Sign* const *signVec, const Id *const *idVec, size_t n)
{
	WrapPointer<Sign, G1> signW(signVec, n);
	WrapPointer<Id, Fr> idW(idVec, n);
	LagrangeInterpolation(getInner().sHm, signW, idW);
}

void Sign::add(const Sign& rhs)
{
	getInner().sHm += rhs.getInner().sHm;
}

bool PublicKey::operator==(const PublicKey& rhs) const
{
	return getInner().sQ == rhs.getInner().sQ;
}

std::ostream& operator<<(std::ostream& os, const PublicKey& pub)
{
	return writeAsHex(os, pub.getInner().sQ);
}

std::istream& operator>>(std::istream& is, PublicKey& pub)
{
	return is >> pub.getInner().sQ;
}

void PublicKey::set(const PublicKeyVec& mpk, const Id& id)
{
	Wrap<PublicKey, G2> w(mpk);
	evalPoly(getInner().sQ, id.getInner().v, w);
}

void PublicKey::set(const PublicKey *const *mpk, size_t k, const Id& id)
{
	WrapPointer<PublicKey, G2> w(mpk, k);
	evalPoly(getInner().sQ, id.getInner().v, w);
}

void PublicKey::recover(const PublicKeyVec& pubVec, const IdVec& idVec)
{
	Wrap<PublicKey, G2> pubW(pubVec);
	Wrap<Id, Fr> idW(idVec);
	LagrangeInterpolation(getInner().sQ, pubW, idW);
}
void PublicKey::recover(const PublicKey *const *pubVec, const Id *const *idVec, size_t n)
{
	WrapPointer<PublicKey, G2> pubW(pubVec, n);
	WrapPointer<Id, Fr> idW(idVec, n);
	LagrangeInterpolation(getInner().sQ, pubW, idW);
}

void PublicKey::add(const PublicKey& rhs)
{
	getInner().sQ += rhs.getInner().sQ;
}

bool SecretKey::operator==(const SecretKey& rhs) const
{
	return getInner().s == rhs.getInner().s;
}

std::ostream& operator<<(std::ostream& os, const SecretKey& sec)
{
	return writeAsHex(os, sec.getInner().s);
}

std::istream& operator>>(std::istream& is, SecretKey& sec)
{
	return is >> sec.getInner().s;
}

void SecretKey::init()
{
	getInner().s.setRand(getRG());
}

void SecretKey::set(const uint64_t *p)
{
	getInner().s.setArrayMask(p, keySize);
}

void SecretKey::getPublicKey(PublicKey& pub) const
{
	G2::mul(pub.getInner().sQ, getQ(), getInner().s);
}

void SecretKey::sign(Sign& sign, const std::string& m) const
{
	G1 Hm;
	HashAndMapToG1(Hm, m);
	G1::mul(sign.getInner().sHm, Hm, getInner().s);
}

void SecretKey::getPop(Sign& pop) const
{
	PublicKey pub;
	getPublicKey(pub);
	std::string m;
	pub.getInner().sQ.getStr(m);
	sign(pop, m);
}

void SecretKey::getMasterSecretKey(SecretKeyVec& msk, size_t k) const
{
	if (k <= 1) throw cybozu::Exception("bls:SecretKey:getMasterSecretKey:bad k") << k;
	msk.resize(k);
	msk[0] = *this;
	for (size_t i = 1; i < k; i++) {
		msk[i].init();
	}
}

void SecretKey::set(const SecretKeyVec& msk, const Id& id)
{
	Wrap<SecretKey, Fr> w(msk);
	evalPoly(getInner().s, id.getInner().v, w);
}
void SecretKey::set(const SecretKey *const *msk, size_t k, const Id& id)
{
	WrapPointer<SecretKey, Fr> w(msk, k);
	evalPoly(getInner().s, id.getInner().v, w);
}

void SecretKey::recover(const SecretKeyVec& secVec, const IdVec& idVec)
{
	Wrap<SecretKey, Fr> secW(secVec);
	Wrap<Id, Fr> idW(idVec);
	LagrangeInterpolation(getInner().s, secW, idW);
}
void SecretKey::recover(const SecretKey *const *secVec, const Id *const *idVec, size_t n)
{
	WrapPointer<SecretKey, Fr> secW(secVec, n);
	WrapPointer<Id, Fr> idW(idVec, n);
	LagrangeInterpolation(getInner().s, secW, idW);
}

void SecretKey::add(const SecretKey& rhs)
{
	getInner().s += rhs.getInner().s;
}

} // bls
