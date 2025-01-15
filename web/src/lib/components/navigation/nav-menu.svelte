<script lang="ts">
  import { cn } from "$lib/utils";
  import { Button } from "$lib/components/ui/button";
  import { goto } from '$app/navigation';
  import * as Accordion from "$lib/components/ui/accordion";
  import { 
    connections,
    endpoints
  } from '$lib/state';
  import {
    NetworkIcon,
    ListIcon,
    LayoutListIcon,
    SettingsIcon,
    DatabaseIcon,
    KeyRoundIcon,
    ChevronRight
  } from "lucide-svelte";

  let menuOpen = $state(true);

  function toggleMenu() {
    menuOpen = !menuOpen;
  }

  function handleNavigation(path: string) {
    goto(path);
    menuOpen = false;
  }

  const { default: defaultSlot } = $props<{
    default?: () => any;
  }>();
</script>

<div class="fixed top-0 left-0 h-full z-50">
  <aside
    class={cn(
      "bg-secondary border-r border-secondary-foreground/10 w-[220px] h-full flex flex-col transition-transform duration-300 ease-in-out",
      menuOpen ? "translate-x-0" : "-translate-x-full md:translate-x-0"
    )}
  >
    <nav class="p-4 space-y-2">
      <Button 
        variant="ghost" 
        class="w-full justify-start" 
        onclick={() => handleNavigation('/config')}
      >
        <SettingsIcon class="mr-2 h-4 w-4" />
        General
      </Button>

      <Accordion.Root type="single" collapsible>
        <Accordion.Item value="connections">
          <Accordion.Trigger>
            <NetworkIcon class="mr-2 h-4 w-4" />
            Connections
          </Accordion.Trigger>
          <Accordion.Content>
            <div class="pl-6 space-y-2">
              {#each Object.keys(connections) as name}
                <Button 
                  variant="ghost" 
                  class="w-full justify-start text-sm" 
                  onclick={() => handleNavigation(`/config/connections/${name}`)}
                >
                  {name}
                </Button>
              {/each}
              <Button 
                variant="ghost" 
                class="w-full justify-start text-sm" 
                onclick={() => handleNavigation('/config/connections/new')}
              >
                + New Connection
              </Button>
            </div>
          </Accordion.Content>
        </Accordion.Item>

        <Accordion.Item value="endpoints">
          <Accordion.Trigger>
            <ListIcon class="mr-2 h-4 w-4" />
            Endpoints
          </Accordion.Trigger>
          <Accordion.Content>
            <div class="pl-6 space-y-2">
              {#each Object.keys(endpoints) as name}
                <Button 
                  variant="ghost" 
                  class="w-full justify-start text-sm" 
                  onclick={() => handleNavigation(`/config/endpoints/${name}`)}
                >
                  {name}
                </Button>
              {/each}
              <Button 
                variant="ghost" 
                class="w-full justify-start text-sm" 
                onclick={() => handleNavigation('/config/endpoints/new')}
              >
                + New Endpoint
              </Button>
            </div>
          </Accordion.Content>
        </Accordion.Item>
      </Accordion.Root>

      <Button 
        variant="ghost" 
        class="w-full justify-start" 
        onclick={() => handleNavigation('/config/server')}
      >
        <DatabaseIcon class="mr-2 h-4 w-4" />
        Server
      </Button>

      <Button 
        variant="ghost" 
        class="w-full justify-start" 
        onclick={() => handleNavigation('/config/duckdb')}
      >
        <LayoutListIcon class="mr-2 h-4 w-4" />
        DuckDB
      </Button>

      <Button 
        variant="ghost" 
        class="w-full justify-start" 
        onclick={() => handleNavigation('/config/aws-secrets')}
      >
        <KeyRoundIcon class="mr-2 h-4 w-4" />
        AWS Secrets
      </Button>
    </nav>
  </aside>
  <div class="md:pl-[220px]">
    {@render defaultSlot?.()}
  </div>
</div> 