<script lang="ts">
  import { Input } from "$lib/components/ui/input";
  import { Label } from "$lib/components/ui/label";
  import { Button } from "$lib/components/ui/button";
  import { cn } from "$lib/utils";
  import { Plus, Trash } from "lucide-svelte";

  export let className = "";
  export let parameters: Array<{
    name: string;
    type: "string" | "number" | "boolean";
    in: "path" | "query" | "header";
    required: boolean;
    description?: string;
  }> = [];

  export let onUpdate: (params: typeof parameters) => void = () => {};

  function validateParameter(param: typeof parameters[0]): string[] {
    const errors: string[] = [];
    
    if (!param.name) {
      errors.push('Parameter name is required');
    } else if (!/^[a-zA-Z_][a-zA-Z0-9_]*$/.test(param.name)) {
      errors.push('Parameter name must start with a letter or underscore and contain only letters, numbers, and underscores');
    }

    if (param.in === 'path' && !param.required) {
      errors.push('Path parameters must be required');
    }

    const nameCount = parameters.filter(p => p.name === param.name).length;
    if (nameCount > 1) {
      errors.push('Parameter name must be unique');
    }

    return errors;
  }

  let validationErrors: Record<number, string[]> = {};

  function validateAndUpdate() {
    validationErrors = {};
    
    parameters.forEach((param, index) => {
      const errors = validateParameter(param);
      if (errors.length > 0) {
        validationErrors[index] = errors;
      }
    });

    if (Object.keys(validationErrors).length === 0) {
      onUpdate(parameters);
    }
  }

  function addParameter() {
    parameters = [
      ...parameters,
      {
        name: "",
        type: "string",
        in: "query",
        required: false
      }
    ];
    validateAndUpdate();
  }

  function removeParameter(index: number) {
    parameters = parameters.filter((_, i) => i !== index);
    validateAndUpdate();
  }

  function handleChange() {
    validateAndUpdate();
  }
</script>

<div class={cn("space-y-6", className)}>
  <div class="flex justify-between items-center">
    <h3 class="text-lg font-medium">Parameters</h3>
    <Button variant="outline" size="sm" on:click={addParameter}>
      <Plus class="w-4 h-4 mr-2" />
      Add Parameter
    </Button>
  </div>

  {#if parameters.length === 0}
    <div class="text-center py-8 text-muted-foreground">
      No parameters defined. Click "Add Parameter" to create one.
    </div>
  {:else}
    {#each parameters as param, i}
      <div class="grid gap-4 p-4 border rounded-lg">
        <div class="flex justify-between items-start">
          <div class="grid gap-2 flex-1">
            <Label for={`param-name-${i}`}>Parameter Name</Label>
            <Input
              id={`param-name-${i}`}
              bind:value={param.name}
              on:change={handleChange}
              placeholder="e.g. user_id"
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
            <Trash class="w-4 h-4" />
          </Button>
        </div>

        <div class="grid grid-cols-3 gap-4">
          <div class="space-y-2">
            <Label for={`param-type-${i}`}>Type</Label>
            <select
              id={`param-type-${i}`}
              bind:value={param.type}
              on:change={handleChange}
              class="w-full px-3 py-2 border rounded-md"
            >
              <option value="string">String</option>
              <option value="number">Number</option>
              <option value="boolean">Boolean</option>
            </select>
          </div>

          <div class="space-y-2">
            <Label for={`param-in-${i}`}>Location</Label>
            <select
              id={`param-in-${i}`}
              bind:value={param.in}
              on:change={handleChange}
              class="w-full px-3 py-2 border rounded-md"
            >
              <option value="query">Query</option>
              <option value="path">Path</option>
              <option value="header">Header</option>
            </select>
          </div>

          <div class="space-y-2">
            <Label for={`param-required-${i}`}>Required</Label>
            <select
              id={`param-required-${i}`}
              bind:value={param.required}
              on:change={handleChange}
              class="w-full px-3 py-2 border rounded-md"
            >
              <option value={true}>Yes</option>
              <option value={false}>No</option>
            </select>
          </div>
        </div>

        <div class="space-y-2">
          <Label for={`param-desc-${i}`}>Description</Label>
          <Input
            id={`param-desc-${i}`}
            bind:value={param.description}
            on:change={handleChange}
            placeholder="Describe what this parameter does..."
          />
        </div>

        {#if validationErrors[i]}
          <div class="text-destructive text-sm space-y-1">
            {#each validationErrors[i] as error}
              <p>{error}</p>
            {/each}
          </div>
        {/if}
      </div>
    {/each}
  {/if}
</div> 