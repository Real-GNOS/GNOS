export default defineEventHandler(async (event) => {
  const auth = await requireAdmin(event)
  const body = await readBody(event)

  if (!body || !body.content) {
    throw createError({ statusCode: 400, message: '缺少审核内容' })
  }

  const configResult = await queryPg(
    "SELECT value FROM site_configs WHERE key = 'ai_review_provider'"
  )
  const provider = configResult.rows[0]?.value || 'openai'

  const keyResult = await queryPg(
    "SELECT value FROM site_configs WHERE key = 'ai_review_api_key'"
  )
  const apiKey = keyResult.rows[0]?.value

  if (!apiKey) {
    return { approved: true, reason: 'AI审核未配置，已自动通过', provider: 'none' }
  }

  const endpointResult = await queryPg(
    "SELECT value FROM site_configs WHERE key = 'ai_review_endpoint'"
  )
  const endpoint = endpointResult.rows[0]?.value || 'https://api.openai.com/v1/chat/completions'

  const modelResult = await queryPg(
    "SELECT value FROM site_configs WHERE key = 'ai_review_model'"
  )
  const model = modelResult.rows[0]?.value || 'gpt-3.5-turbo'

  try {
    const response = await fetch(endpoint, {
      method: 'POST',
      headers: {
        'Content-Type': 'application/json',
        'Authorization': `Bearer ${apiKey}`,
      },
      body: JSON.stringify({
        model,
        messages: [
          {
            role: 'system',
            content: `你是Cocokalo平台的内容审核助手。请审核用户提交的内容是否符合以下规则：
1. 不包含色情、暴力、恐怖内容
2. 不包含政治敏感内容
3. 不包含人身攻击、辱骂、歧视言论
4. 不包含垃圾广告、诈骗信息
5. 不包含违法内容

请以JSON格式返回审核结果：
{"approved": true/false, "reason": "审核理由", "confidence": 0.0-1.0}`
          },
          {
            role: 'user',
            content: `请审核以下内容：\n${body.content.slice(0, 2000)}`
          }
        ],
        temperature: 0.3,
        max_tokens: 200,
      }),
    })

    if (!response.ok) {
      console.error('AI review API error:', response.status)
      return { approved: true, reason: 'AI审核服务暂不可用，已自动通过', provider }
    }

    const data = await response.json()
    const aiContent = data.choices?.[0]?.message?.content || ''

    const jsonMatch = aiContent.match(/\{[\s\S]*\}/)
    if (jsonMatch) {
      const result = JSON.parse(jsonMatch[0])
      return {
        approved: result.approved !== false,
        reason: result.reason || '审核完成',
        confidence: result.confidence || 0.5,
        provider,
      }
    }

    return { approved: true, reason: 'AI审核结果解析失败，已自动通过', provider }
  } catch (err) {
    console.error('AI review error:', err)
    return { approved: true, reason: 'AI审核异常，已自动通过', provider: 'error' }
  }
})
