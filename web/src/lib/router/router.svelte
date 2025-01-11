<script lang="ts">
  import { currentPath, routes } from './index';
  import GeneralConfig from '../../routes/config/+page.svelte';
  import Endpoints from '../../routes/config/endpoints/+page.svelte';
  import Connections from '../../routes/config/connections/+page.svelte';
  import Welcome from '../../routes/+page.svelte';

  // Define routes
  $routes = [
    { path: '/', component: Welcome },
    { path: '/config', component: GeneralConfig },
    { path: '/config/endpoints', component: Endpoints },
    { path: '/config/connections', component: Connections }
  ];

  // Find current route
  $: currentRoute = $routes.find(route => 
    $currentPath === route.path || 
    $currentPath.startsWith(route.path + '?')
  ) || $routes[0];
</script>

<svelte:component this={currentRoute.component} /> 