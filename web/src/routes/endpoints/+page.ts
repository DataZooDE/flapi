import type { PageLoad } from './$types';
import { getEndpoints } from '$lib/api';

export const load: PageLoad = async ({ fetch }) => {
  const endpoints = await getEndpoints(fetch);
  return { endpoints };
};
