/*
 * Copyright (c) 2016 Laura Garcia <nevola@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 */

#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/netlink.h>
#include <linux/netfilter.h>
#include <linux/netfilter/nf_tables.h>
#include <linux/static_key.h>
#include <net/netfilter/nf_tables.h>
#include <net/netfilter/nf_tables_core.h>

static DEFINE_PER_CPU(struct rnd_state, nft_numgen_prandom_state);

struct nft_ng_inc {
	enum nft_registers      dreg:8;
	u32			modulus;
	atomic_t		counter;
	u32			offset;
	struct nft_set		*map;
};

static u32 nft_ng_inc_gen(struct nft_ng_inc *priv)
{
	u32 nval, oval;

	do {
		oval = atomic_read(&priv->counter);
		nval = (oval + 1 < priv->modulus) ? oval + 1 : 0;
	} while (atomic_cmpxchg(&priv->counter, oval, nval) != oval);

	return nval + priv->offset;
}

static void nft_ng_inc_eval(const struct nft_expr *expr,
			    struct nft_regs *regs,
			    const struct nft_pktinfo *pkt)
{
	struct nft_ng_inc *priv = nft_expr_priv(expr);

	regs->data[priv->dreg] = nft_ng_inc_gen(priv);
}

static void nft_ng_inc_map_eval(const struct nft_expr *expr,
				struct nft_regs *regs,
				const struct nft_pktinfo *pkt)
{
	struct nft_ng_inc *priv = nft_expr_priv(expr);
	const struct nft_set *map = priv->map;
	const struct nft_set_ext *ext;
	u32 result;
	bool found;

	result = nft_ng_inc_gen(priv);
	found = map->ops->lookup(nft_net(pkt), map, &result, &ext);

	if (!found)
		return;

	nft_data_copy(&regs->data[priv->dreg],
		      nft_set_ext_data(ext), map->dlen);
}

static const struct nla_policy nft_ng_policy[NFTA_NG_MAX + 1] = {
	[NFTA_NG_DREG]		= { .type = NLA_U32 },
	[NFTA_NG_MODULUS]	= { .type = NLA_U32 },
	[NFTA_NG_TYPE]		= { .type = NLA_U32 },
	[NFTA_NG_OFFSET]	= { .type = NLA_U32 },
	[NFTA_NG_SET_NAME]	= { .type = NLA_STRING,
				    .len = NFT_SET_MAXNAMELEN - 1 },
	[NFTA_NG_SET_ID]	= { .type = NLA_U32 },
};

static int nft_ng_inc_init(const struct nft_ctx *ctx,
			   const struct nft_expr *expr,
			   const struct nlattr * const tb[])
{
	struct nft_ng_inc *priv = nft_expr_priv(expr);

	if (tb[NFTA_NG_OFFSET])
		priv->offset = ntohl(nla_get_be32(tb[NFTA_NG_OFFSET]));

	priv->modulus = ntohl(nla_get_be32(tb[NFTA_NG_MODULUS]));
	if (priv->modulus == 0)
		return -ERANGE;

	if (priv->offset + priv->modulus - 1 < priv->offset)
		return -EOVERFLOW;

	priv->dreg = nft_parse_register(tb[NFTA_NG_DREG]);
	atomic_set(&priv->counter, priv->modulus - 1);

	return nft_validate_register_store(ctx, priv->dreg, NULL,
					   NFT_DATA_VALUE, sizeof(u32));
}

static int nft_ng_inc_map_init(const struct nft_ctx *ctx,
			       const struct nft_expr *expr,
			       const struct nlattr * const tb[])
{
	struct nft_ng_inc *priv = nft_expr_priv(expr);
	u8 genmask = nft_genmask_next(ctx->net);

	nft_ng_inc_init(ctx, expr, tb);

	priv->map = nft_set_lookup_global(ctx->net, ctx->table,
					  tb[NFTA_NG_SET_NAME],
					  tb[NFTA_NG_SET_ID], genmask);

	if (IS_ERR(priv->map))
		return PTR_ERR(priv->map);

	return 0;
}

static int nft_ng_dump(struct sk_buff *skb, enum nft_registers dreg,
		       u32 modulus, enum nft_ng_types type, u32 offset)
{
	if (nft_dump_register(skb, NFTA_NG_DREG, dreg))
		goto nla_put_failure;
	if (nla_put_be32(skb, NFTA_NG_MODULUS, htonl(modulus)))
		goto nla_put_failure;
	if (nla_put_be32(skb, NFTA_NG_TYPE, htonl(type)))
		goto nla_put_failure;
	if (nla_put_be32(skb, NFTA_NG_OFFSET, htonl(offset)))
		goto nla_put_failure;

	return 0;

nla_put_failure:
	return -1;
}

static int nft_ng_inc_dump(struct sk_buff *skb, const struct nft_expr *expr)
{
	const struct nft_ng_inc *priv = nft_expr_priv(expr);

	return nft_ng_dump(skb, priv->dreg, priv->modulus, NFT_NG_INCREMENTAL,
			   priv->offset);
}

static int nft_ng_inc_map_dump(struct sk_buff *skb,
			       const struct nft_expr *expr)
{
	const struct nft_ng_inc *priv = nft_expr_priv(expr);

	if (nft_ng_dump(skb, priv->dreg, priv->modulus,
			NFT_NG_INCREMENTAL, priv->offset) ||
	    nla_put_string(skb, NFTA_NG_SET_NAME, priv->map->name))
		goto nla_put_failure;

	return 0;

nla_put_failure:
	return -1;
}

struct nft_ng_random {
	enum nft_registers      dreg:8;
	u32			modulus;
	u32			offset;
	struct nft_set		*map;
};

static u32 nft_ng_random_gen(struct nft_ng_random *priv)
{
	struct rnd_state *state = this_cpu_ptr(&nft_numgen_prandom_state);

	return reciprocal_scale(prandom_u32_state(state), priv->modulus) +
	       priv->offset;
}

static void nft_ng_random_eval(const struct nft_expr *expr,
			       struct nft_regs *regs,
			       const struct nft_pktinfo *pkt)
{
	struct nft_ng_random *priv = nft_expr_priv(expr);

	regs->data[priv->dreg] = nft_ng_random_gen(priv);
}

static void nft_ng_random_map_eval(const struct nft_expr *expr,
				   struct nft_regs *regs,
				   const struct nft_pktinfo *pkt)
{
	struct nft_ng_random *priv = nft_expr_priv(expr);
	const struct nft_set *map = priv->map;
	const struct nft_set_ext *ext;
	u32 result;
	bool found;

	result = nft_ng_random_gen(priv);
	found = map->ops->lookup(nft_net(pkt), map, &result, &ext);
	if (!found)
		return;

	nft_data_copy(&regs->data[priv->dreg],
		      nft_set_ext_data(ext), map->dlen);
}

static int nft_ng_random_init(const struct nft_ctx *ctx,
			      const struct nft_expr *expr,
			      const struct nlattr * const tb[])
{
	struct nft_ng_random *priv = nft_expr_priv(expr);

	if (tb[NFTA_NG_OFFSET])
		priv->offset = ntohl(nla_get_be32(tb[NFTA_NG_OFFSET]));

	priv->modulus = ntohl(nla_get_be32(tb[NFTA_NG_MODULUS]));
	if (priv->modulus == 0)
		return -ERANGE;

	if (priv->offset + priv->modulus - 1 < priv->offset)
		return -EOVERFLOW;

	prandom_init_once(&nft_numgen_prandom_state);

	priv->dreg = nft_parse_register(tb[NFTA_NG_DREG]);

	return nft_validate_register_store(ctx, priv->dreg, NULL,
					   NFT_DATA_VALUE, sizeof(u32));
}

static int nft_ng_random_map_init(const struct nft_ctx *ctx,
				  const struct nft_expr *expr,
				  const struct nlattr * const tb[])
{
	struct nft_ng_random *priv = nft_expr_priv(expr);
	u8 genmask = nft_genmask_next(ctx->net);

	nft_ng_random_init(ctx, expr, tb);
	priv->map = nft_set_lookup_global(ctx->net, ctx->table,
					  tb[NFTA_NG_SET_NAME],
					  tb[NFTA_NG_SET_ID], genmask);
	if (IS_ERR(priv->map))
		return PTR_ERR(priv->map);

	return 0;
}

static int nft_ng_random_dump(struct sk_buff *skb, const struct nft_expr *expr)
{
	const struct nft_ng_random *priv = nft_expr_priv(expr);

	return nft_ng_dump(skb, priv->dreg, priv->modulus, NFT_NG_RANDOM,
			   priv->offset);
}

static int nft_ng_random_map_dump(struct sk_buff *skb,
				  const struct nft_expr *expr)
{
	const struct nft_ng_random *priv = nft_expr_priv(expr);

	if (nft_ng_dump(skb, priv->dreg, priv->modulus,
			NFT_NG_RANDOM, priv->offset) ||
	    nla_put_string(skb, NFTA_NG_SET_NAME, priv->map->name))
		goto nla_put_failure;

	return 0;

nla_put_failure:
	return -1;
}

static struct nft_expr_type nft_ng_type;
static const struct nft_expr_ops nft_ng_inc_ops = {
	.type		= &nft_ng_type,
	.size		= NFT_EXPR_SIZE(sizeof(struct nft_ng_inc)),
	.eval		= nft_ng_inc_eval,
	.init		= nft_ng_inc_init,
	.dump		= nft_ng_inc_dump,
};

static const struct nft_expr_ops nft_ng_inc_map_ops = {
	.type		= &nft_ng_type,
	.size		= NFT_EXPR_SIZE(sizeof(struct nft_ng_inc)),
	.eval		= nft_ng_inc_map_eval,
	.init		= nft_ng_inc_map_init,
	.dump		= nft_ng_inc_map_dump,
};

static const struct nft_expr_ops nft_ng_random_ops = {
	.type		= &nft_ng_type,
	.size		= NFT_EXPR_SIZE(sizeof(struct nft_ng_random)),
	.eval		= nft_ng_random_eval,
	.init		= nft_ng_random_init,
	.dump		= nft_ng_random_dump,
};

static const struct nft_expr_ops nft_ng_random_map_ops = {
	.type		= &nft_ng_type,
	.size		= NFT_EXPR_SIZE(sizeof(struct nft_ng_random)),
	.eval		= nft_ng_random_map_eval,
	.init		= nft_ng_random_map_init,
	.dump		= nft_ng_random_map_dump,
};

static const struct nft_expr_ops *
nft_ng_select_ops(const struct nft_ctx *ctx, const struct nlattr * const tb[])
{
	u32 type;

	if (!tb[NFTA_NG_DREG]	 ||
	    !tb[NFTA_NG_MODULUS] ||
	    !tb[NFTA_NG_TYPE])
		return ERR_PTR(-EINVAL);

	type = ntohl(nla_get_be32(tb[NFTA_NG_TYPE]));

	switch (type) {
	case NFT_NG_INCREMENTAL:
		if (tb[NFTA_NG_SET_NAME])
			return &nft_ng_inc_map_ops;
		return &nft_ng_inc_ops;
	case NFT_NG_RANDOM:
		if (tb[NFTA_NG_SET_NAME])
			return &nft_ng_random_map_ops;
		return &nft_ng_random_ops;
	}

	return ERR_PTR(-EINVAL);
}

static struct nft_expr_type nft_ng_type __read_mostly = {
	.name		= "numgen",
	.select_ops	= nft_ng_select_ops,
	.policy		= nft_ng_policy,
	.maxattr	= NFTA_NG_MAX,
	.owner		= THIS_MODULE,
};

static int __init nft_ng_module_init(void)
{
	return nft_register_expr(&nft_ng_type);
}

static void __exit nft_ng_module_exit(void)
{
	nft_unregister_expr(&nft_ng_type);
}

module_init(nft_ng_module_init);
module_exit(nft_ng_module_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Laura Garcia <nevola@gmail.com>");
MODULE_ALIAS_NFT_EXPR("numgen");
