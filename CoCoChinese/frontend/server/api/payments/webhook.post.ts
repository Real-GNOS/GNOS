export default defineEventHandler(async (event) => {
  const provider = process.env.PAYMENT_PROVIDER || 'stripe'
  const signature = getHeader(event, 'stripe-signature')

  if (provider === 'stripe' && signature) {
    const stripeKey = process.env.STRIPE_SECRET_KEY
    const webhookSecret = process.env.STRIPE_WEBHOOK_SECRET
    if (stripeKey && webhookSecret) {
      try {
        const stripe = await import('stripe').then(m => new m.default(stripeKey))
        const body = await readRawBody(event)
        const sig = stripe.webhooks.constructEvent(body!, signature, webhookSecret)
        const { queryPg } = await import('../../utils/pg')

        if (sig.type === 'checkout.session.completed') {
          const session = sig.data.object as any
          const userId = parseInt(session.metadata.userId)
          const plan = session.metadata.plan

          await queryPg(
            'INSERT INTO payments (user_id, amount, currency, status, provider, provider_id, plan) VALUES ($1, $2, $3, $4, $5, $6, $7)',
            [userId, session.amount_total, session.currency, 'completed', 'stripe', session.id, plan]
          )

          // Upsert subscription
          await queryPg(`
            INSERT INTO subscriptions (user_id, plan, status, provider, provider_id, current_period_start, current_period_end)
            VALUES ($1, $2, 'active', 'stripe', $3, to_timestamp($4), to_timestamp($5))
            ON CONFLICT (user_id) DO UPDATE SET
              plan = EXCLUDED.plan,
              status = 'active',
              provider_id = EXCLUDED.provider_id,
              current_period_start = EXCLUDED.current_period_start,
              current_period_end = EXCLUDED.current_period_end
          `, [userId, plan, session.subscription, session.created, Math.floor(Date.now() / 1000) + 2592000])
        }

        return { received: true }
      } catch (e: any) {
        throw createError({ statusCode: 400, message: `Webhook error: ${e.message}` })
      }
    }
  }

  return { received: true }
})
