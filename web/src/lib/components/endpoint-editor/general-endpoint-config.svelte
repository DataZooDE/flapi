<script lang="ts">
  import { Input } from "$lib/components/ui/input";
  import { Label } from "$lib/components/ui/label";
  import { Button } from "$lib/components/ui/button";
  import { Textarea } from "$lib/components/ui/textarea";
  import { cn } from "$lib/utils";

  export let className = "";
  export let path = "";
  export let method: "GET" | "POST" | "PUT" | "DELETE" | "PATCH" = "GET";
  export let version: "v1" = "v1";
  export let description = "";

  const httpMethods = ["GET", "POST", "PUT", "DELETE", "PATCH"] as const;
  const apiVersions = ["v1"] as const;

  export let onUpdate: (values: {
    path: string;
    method: typeof httpMethods[number];
    version: typeof apiVersions[number];
    description: string;
  }) => void = () => {};

  function handleChange() {
    onUpdate({ 
      path, 
      method: method as typeof httpMethods[number],
      version: version as typeof apiVersions[number],
      description 
    });
  }
</script>

<div class={cn("space-y-6", className)}>
  <div class="grid grid-cols-[1fr_200px] gap-4">
    <div class="space-y-2">
      <Label for_id="path">Endpoint Path</Label>
      <Input 
        id="path"
        bind:value={path}
        on:change={handleChange}
        placeholder="/api/users/123"
        pattern="^/.*"
        title="Path must start with /"
      />
    </div>
    
    <div class="space-y-2">
      <Label for_id="method">HTTP Method</Label>
      <select
        id="method"
        bind:value={method}
        on:change={handleChange}
        class="w-full px-3 py-2 border rounded-md"
      >
        {#each httpMethods as m}
          <option value={m}>{m}</option>
        {/each}
      </select>
    </div>
  </div>

  <div class="space-y-2">
    <Label for_id="version">API Version</Label>
    <select
      id="version"
      bind:value={version}
      on:change={handleChange}
      class="w-full px-3 py-2 border rounded-md"
    >
      {#each apiVersions as v}
        <option value={v}>{v}</option>
      {/each}
    </select>
  </div>

  <div class="space-y-2">
    <Label for_id="description">Description</Label>
    <Textarea
      id="description"
      bind:value={description}
      on:change={handleChange}
      placeholder="Describe what this endpoint does..."
      rows={4}
    />
  </div>
</div> 