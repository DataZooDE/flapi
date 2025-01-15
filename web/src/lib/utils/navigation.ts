import { goto as svelteGoto } from '$app/navigation';

export async function goto(path: string) {
  await svelteGoto(path);
} 