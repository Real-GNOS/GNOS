import { getCategories } from '../../../utils/forum'

export default defineEventHandler(async () => {
  const categories = await getCategories()
  return { success: true, data: categories }
})
