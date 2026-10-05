export default defineEventHandler(async (event) => {
  const auth = await requireAuth(event)
  const { plan } = await readBody(event)

  if (!['monthly', 'yearly', 'lifetime'].includes(plan)) {
    throw createError({ statusCode: 400, message: '无效的套餐' })
  }

  const prices: Record<string, number> = {
    monthly: 1500,
    yearly: 12800,
    lifetime: 19800,
  }

  const amount = prices[plan]
  if (!amount) throw createError({ statusCode: 400, message: '无效的套餐' })

  const provider = process.env.PAYMENT_PROVIDER || 'stripe'

  try {
    if (provider === 'stripe') {
      const stripeKey = process.env.STRIPE_SECRET_KEY
      if (stripeKey) {
        const stripe = await import('stripe').then(m => new m.default(stripeKey))
        const session = await stripe.checkout.sessions.create({
          mode: 'subscription',
          payment_method_types: ['card'],
          line_items: [{ price: process.env[`STRIPE_PRICE_${plan.toUpperCase()}`], quantity: 1 }],
          customer_email: auth.username,
          success_url: `${process.env.SITE_URL || 'http://localhost:3000'}/vip/success`,
          cancel_url: `${process.env.SITE_URL || 'http://localhost:3000'}/vip`,
          metadata: { userId: String(auth.userId), plan },
        })
        return { url: session.url, sessionId: session.id }
      }
    }

    // Fallback to manual payment record
    const { queryPg } = await import('../../utils/pg')
    await queryPg(
      'INSERT INTO payments (user_id, amount, currency, status, provider, plan) VALUES ($1, $2, $3, $4, $5, $6)',
      [auth.userId, amount, 'cny', 'pending', 'manual', plan]
    )

    return { success: true, message: '订单已创建，请联系管理员完成支付' }
  } catch (e: any) {
    throw createError({ statusCode: 500, message: `支付创建失败: ${e.message}` })
  }
})
