<script lang="ts">
  import { Button } from "$lib/components/ui/button";
  import * as DropdownMenu from "$lib/components/ui/dropdown-menu";
  import { Sun, Moon, Laptop } from "lucide-svelte";
  import { theme } from "$lib/stores/theme";
  import type { Theme } from "$lib/config/theme";

  function setTheme(newTheme: Theme) {
    theme.set(newTheme);
  }
</script>

<DropdownMenu.Root>
  <DropdownMenu.Trigger asChild>
    <Button variant="ghost" size="icon">
      <Sun class="h-[1.2rem] w-[1.2rem] rotate-0 scale-100 transition-all dark:-rotate-90 dark:scale-0" />
      <Moon class="absolute h-[1.2rem] w-[1.2rem] rotate-90 scale-0 transition-all dark:rotate-0 dark:scale-100" />
      <span class="sr-only">Toggle theme</span>
    </Button>
  </DropdownMenu.Trigger>
  <DropdownMenu.Content class="w-32">
    <DropdownMenu.Item class="cursor-pointer" on:click={() => setTheme('light')}>
      <Sun class="mr-2 h-4 w-4" />
      <span>Light</span>
    </DropdownMenu.Item>
    <DropdownMenu.Item class="cursor-pointer" on:click={() => setTheme('dark')}>
      <Moon class="mr-2 h-4 w-4" />
      <span>Dark</span>
    </DropdownMenu.Item>
    <DropdownMenu.Item class="cursor-pointer" on:click={() => setTheme('system')}>
      <Laptop class="mr-2 h-4 w-4" />
      <span>System</span>
    </DropdownMenu.Item>
  </DropdownMenu.Content>
</DropdownMenu.Root> 