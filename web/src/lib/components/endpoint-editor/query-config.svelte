<script lang="ts">
  import { Input } from "$lib/components/ui/input";
  import { Label } from "$lib/components/ui/label";
  import { Button } from "$lib/components/ui/button";
  import { Select, type SelectProps } from "$lib/components/ui/select";
  import { cn } from "$lib/utils";
  import { PlusIcon, TrashIcon } from "lucide-svelte";

  export let className = "";
  
  interface QueryConfig {
    sql: string;
    parameters: Array<{
      name: string;
      type: "string" | "number" | "boolean";
      required: boolean;
    }>;
  }

  export let query: QueryConfig = {
    sql: "",
    parameters: []
  };

  export let onUpdate: (q: QueryConfig) => void = () => {};

  function validateQuery(): string[] {
    const errors: string[] = [];
    
    if (!query.sql.trim()) {
      errors.push('SQL query is required');
    }

    // Check for SQL injection vulnerabilities
    if (/;\s*$/.test(query.sql)) {
      errors.push('Query should not end with semicolon');
    }

    // Validate parameter references
    const paramRefs = query.sql.match(/\$\{([^}]+)\}/g) || [];
    const paramNames = new Set(query.parameters.map(p => p.name));
    
    paramRefs.forEach(ref => {
      const name = ref.slice(2, -1);
      if (!paramNames.has(name)) {
        errors.push(`Parameter "${name}" referenced in query but not defined`);
      }
    });

    return errors;
  }

  let validationErrors: string[] = [];

  function validateAndUpdate() {
    validationErrors = validateQuery();
    if (validationErrors.length === 0) {
      onUpdate(query);
    }
  }

  function addParameter() {
    query.parameters = [
      ...query.parameters,
      {
        name: "",
        type: "string",
        required: false
      }
    ];
    validateAndUpdate();
  }

  function removeParameter(index: number) {
    query.parameters = query.parameters.filter((_, i) => i !== index);
    validateAndUpdate();
  }

  function handleChange() {
    validateAndUpdate();
  }
</script>

<div class={cn("space-y-6", className)}>
  <div class="flex justify-between items-center">
    <h3 class="text-lg font-medium">Query Configuration</h3>
  </div>

  <div class="space-y-4">
    <div class="space-y-2">
      <Label for="sql-query">SQL Query</Label>
      <textarea
        id="sql-query"
        bind:value={query.sql}
        on:change={handleChange}
        class="w-full h-32 px-3 py-2 border rounded-md font-mono"
        placeholder="SELECT * FROM users WHERE id = $&#123;userId&#125;"
      ></textarea>
    </div>

    <div class="space-y-4">
      <div class="flex justify-between items-center">
        <h4 class="font-medium">Query Parameters</h4>
        <Button variant="outline" size="sm" on:click={addParameter}>
          <PlusIcon class="w-4 h-4 mr-2" />
          Add Parameter
        </Button>
      </div>

      {#if query.parameters.length === 0}
        <div class="text-center py-8 text-muted-foreground">
          No parameters defined. Click "Add Parameter" to create one.
        </div>
      {:else}
        {#each query.parameters as param, i}
          <div class="grid gap-4 p-4 border rounded-lg">
            <div class="flex justify-between items-start">
              <div class="grid gap-2 flex-1">
                <Label for={`query-param-name-${i}`}>Parameter Name</Label>
                <Input
                  id={`query-param-name-${i}`}
                  bind:value={param.name}
                  on:change={handleChange}
                  placeholder="e.g. userId"
                />
              </div>
              <Button 
                variant="ghost" 
                size="icon"
                class="text-destructive hover:text-destructive"
                on:click={() => removeParameter(i)}
                title="Remove parameter"
                aria-label="Remove parameter"
              >
                <TrashIcon class="w-4 h-4" />
              </Button>
            </div>

            <div class="grid grid-cols-2 gap-4">
              <div class="space-y-2">
                <Label for={`query-param-type-${i}`}>Type</Label>
                <select
                  class="w-full px-3 py-2 border rounded-md"
                  bind:value={param.type}
                  on:change={handleChange}
                >
                  <option value="string">String</option>
                  <option value="number">Number</option>
                  <option value="boolean">Boolean</option>
                </select>
              </div>

              <div class="space-y-2">
                <Label for={`query-param-required-${i}`}>Required</Label>
                <select
                  class="w-full px-3 py-2 border rounded-md"
                  bind:value={param.required}
                  on:change={handleChange}
                >
                  <option value={true}>Yes</option>
                  <option value={false}>No</option>
                </select>
              </div>
            </div>
          </div>
        {/each}
      {/if}
    </div>

    {#if validationErrors.length > 0}
      <div class="text-destructive text-sm space-y-1">
        {#each validationErrors as error}
          <p>{error}</p>
        {/each}
      </div>
    {/if}
  </div>
</div> 